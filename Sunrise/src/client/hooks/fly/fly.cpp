/**
 * Velocity fly. The movement keys and the left stick set the player's velocity every tick.
 * Velocity is set, not added. Adding compounds each tick and leaves gravity in the vertical lane.
 * Setting it means releasing every key stops the player, which is what holds a hover.
 * The write goes in before the simulation step, and again before the sync that publishes it.
 * Movement speed reuses the same keys and direction with fly off, adds the controller's left
 * stick through the move vector the game derives after its controller backends (no device is
 * polled here), and writes only the horizontal lanes at the configured speed, leaving the
 * vertical lane to the game. It only raises a horizontal move the game is already making, and
 * only for one step: the game's own lanes go back after it.
 */

#include "fly.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/runtime/runtime.h"
#include "../../input/window_focus.h"
#include "../../movement/movement_settings_store.h"
#include "../../patterns/image_scan.h"
#include "../noclip/runtime.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::fly {
namespace {

namespace bindings = state::account::settings::bindings;

/** The high bit of a polled key state marks it held. */
constexpr SHORT kKeyHeldBit = static_cast<SHORT>(0x8000);
/** Below this squared length a direction counts as none. */
constexpr float kMinimumLengthSquared = 0.000001F;

/** The horizontal lanes. The basis is X forward, Z up. */
constexpr std::size_t kLaneX = 0;
constexpr std::size_t kLaneY = 1;

/**
 * The stick finalize. It takes the left stick after the game's own deadzone, layout and
 * inversion, and writes the move vector into the per-player input context. Anchored on its
 * prologue, the context array it addresses and the player stride it multiplies by.
 */
constexpr std::string_view kStickFinalizeText =
    "48 89 5C 24 10 57 48 83 EC 50 0F 29 74 24 40 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 38 "
    "48 63 C1 48 8D 3D ? ? ? ? 48 69 D8 E0 4C 00 00 48 8D 97 70 19 00 00";
/** Compiled pattern bytes of the stick finalize signature. */
constexpr auto kStickFinalize =
    patterns::signature<patterns::signature_length(kStickFinalizeText)>(kStickFinalizeText);
/** `lea rdi,[rip+disp32]` of the player input context array: its operand, and the end of it. */
constexpr std::size_t kContextOperandOffset = 0x24;
constexpr std::size_t kContextInstructionEnd = 0x28;
/** One player input context. The `imul` in the signature carries the same stride. */
constexpr std::size_t kPlayerContextStride = 0x4CE0;
/** The move vector inside a context: forward, then left. Both already in [-1, 1]. */
constexpr std::size_t kContextMoveOffset = 0x1980;
/** The one local player. */
constexpr std::size_t kLocalPlayer = 0;
/** Above this squared length a travel vector is brought back to unit length. */
constexpr float kMaximumLengthSquared = 1.0F;
/**
 * Native horizontal speed, squared, at or below which the game has the player standing. Only
 * numerical noise sits under it; noclip treats a body below the same bar as stationary.
 */
constexpr float kNativeStationarySquared = 0.000001F;

/** The left stick as the game finalised it: forward and left, each in [-1, 1]. */
struct StickMove {
    float forward{};
    float left{};
};

/** One movement direction in the player's own frame. */
enum class Direction : std::size_t {
    forward,
    backward,
    left,
    right,
    up,
    down,
    count,
};

/** Sizes the per-direction flag array. */
constexpr std::size_t kDirectionCount = static_cast<std::size_t>(Direction::count);

/** One polled action and the direction it moves. */
struct ActionDirection {
    bindings::Action action;
    Direction direction;
};

/** Crouch has two actions and either one descends. Both are read as a hold, to pair with jump. */
constexpr std::array<ActionDirection, 7> kActions{{
    {bindings::Action::moveForward, Direction::forward},
    {bindings::Action::moveBackward, Direction::backward},
    {bindings::Action::moveLeft, Direction::left},
    {bindings::Action::moveRight, Direction::right},
    {bindings::Action::jump, Direction::up},
    {bindings::Action::toggleCrouch, Direction::down},
    {bindings::Action::holdCrouch, Direction::down},
}};

/** Both binding halves of each polled action, in the order of the table above. */
std::array<bindings::Binding, kActions.size()> g_bindings{};
/** Set once the bindings are read, so the costly account snapshot runs once. */
bool g_bindingsRead{false};
/** The toggle key on the previous frame, so the switch only flips on the press. */
std::atomic_bool g_toggleDown{false};
/** The height the body had before the step, which is the height to hold. */
float g_heightBeforeStep{0.0F};
bool g_heightValid{false};
/** Set while a press owns the vertical lane. The hold stands aside. */
bool g_steered{false};
/** The movement speed toggle key on the previous frame, so its switch only flips on the press. */
std::atomic_bool g_speedToggleDown{false};
/** The player input context array, or null while the stick has no source. */
std::atomic<std::byte*> g_playerContexts{nullptr};

/**
 * The game's own horizontal lanes, taken before movement speed replaced them for one step, and the
 * body they belong to. Put back after that step, so the raised speed never outlives it.
 */
struct NativeHorizontal {
    void* body{};
    float x{};
    float y{};
};
/** Filled only while a step carries movement speed's lanes. A null body: nothing to restore. */
NativeHorizontal g_speedNative{};

/**
 * Takes the account's movement bindings once they are loaded.
 * @return True when they have been read.
 */
[[nodiscard]] bool read_bindings() noexcept {
    if (g_bindingsRead) {
        return true;
    }
    // The snapshot copies the whole account, so it stops once the bindings arrive.
    const state::AccountState account = state::account_snapshot();
    if (!account.settings.keyBindings.configured) {
        return false;
    }
    for (std::size_t index = 0; index < kActions.size(); ++index) {
        const std::size_t action = static_cast<std::size_t>(kActions[index].action);
        g_bindings[index] = account.settings.keyBindings.values[action];
    }
    g_bindingsRead = true;
    return true;
}

/**
 * A half is an input code, not a virtual key. A half bound to a mouse button gives no key.
 * @param half One binding half, empty when unbound.
 * @return True while its key is down.
 */
[[nodiscard]] bool half_down(const std::optional<std::uint16_t>& half) noexcept {
    if (!half.has_value()) {
        return false;
    }
    const std::uint32_t key = teleport::action_key(*half);
    return key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & kKeyHeldBit) != 0;
}

/** @return One flag per direction, true while any key bound to it is down. */
[[nodiscard]] std::array<bool, kDirectionCount> pressed_directions() noexcept {
    std::array<bool, kDirectionCount> pressed{};
    for (std::size_t index = 0; index < kActions.size(); ++index) {
        const bindings::Binding& binding = g_bindings[index];
        if (half_down(binding.primary) || half_down(binding.secondary)) {
            pressed[static_cast<std::size_t>(kActions[index].direction)] = true;
        }
    }
    return pressed;
}

/**
 * Reads the local player's left stick as the game finalised it. The field is image data, so it is
 * always mapped; a torn read only moves the player for one tick.
 * @return The stick, or zeroes while it has no source or the field is out of its range.
 */
[[nodiscard]] StickMove stick_move() noexcept {
    std::byte* const contexts = g_playerContexts.load(std::memory_order_acquire);
    if (contexts == nullptr) {
        return StickMove{};
    }
    StickMove move{};
    std::memcpy(&move,
                contexts + kLocalPlayer * kPlayerContextStride + kContextMoveOffset,
                sizeof move);
    // The game clamps both lanes to [-1, 1]. Anything else, NaN included, is not a stick.
    if (!(std::fabs(move.forward) <= 1.0F) || !(std::fabs(move.left) <= 1.0F)) {
        return StickMove{};
    }
    return move;
}

/**
 * Forward turned about the up axis. This turn is the player's right, confirmed in flight.
 * @return The strafe axis, or zeroes when the camera looks straight up or down.
 */
[[nodiscard]] teleport::Vector right_of(const teleport::Vector& forward) noexcept {
    teleport::Vector right{forward[kLaneY], -forward[kLaneX], 0.0F};
    const float lengthSquared = right[kLaneX] * right[kLaneX] + right[kLaneY] * right[kLaneY];
    if (lengthSquared <= kMinimumLengthSquared) {
        return teleport::Vector{};
    }
    const float length = std::sqrt(lengthSquared);
    right[kLaneX] /= length;
    right[kLaneY] /= length;
    return right;
}

/**
 * Composes the pressed directions and, when requested, the stick into one vector of at most unit
 * length. With a zero stick this is the unit vector of the keys alone.
 * @param pressed One flag per direction.
 * @param stick The left stick, forward and left.
 * @param forward Camera forward vector.
 * @return The direction to move, or all zeroes when nothing is pressed.
 */
[[nodiscard]] teleport::Vector travel(const std::array<bool, kDirectionCount>& pressed,
                                      const StickMove& stick,
                                      const teleport::Vector& forward) noexcept {
    const teleport::Vector right = right_of(forward);
    teleport::Vector move{};
    const auto add = [&move](const teleport::Vector& axis, float scale) noexcept {
        for (std::size_t lane = 0; lane < teleport::kVectorLanes; ++lane) {
            move[lane] += axis[lane] * scale;
        }
    };
    if (pressed[static_cast<std::size_t>(Direction::forward)]) {
        add(forward, 1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::backward)]) {
        add(forward, -1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::right)]) {
        add(right, 1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::left)]) {
        add(right, -1.0F);
    }
    // The stick's second lane is left, so it turns the other way along the strafe axis.
    add(forward, stick.forward);
    add(right, -stick.left);
    if (pressed[static_cast<std::size_t>(Direction::up)]) {
        move[teleport::kVerticalLane] += 1.0F;
    }
    if (pressed[static_cast<std::size_t>(Direction::down)]) {
        move[teleport::kVerticalLane] -= 1.0F;
    }
    float lengthSquared = 0.0F;
    for (const float lane : move) {
        lengthSquared += lane * lane;
    }
    if (lengthSquared <= kMinimumLengthSquared) {
        return teleport::Vector{};
    }
    // A key is a whole lane, so any press is brought to unit length exactly as before: a diagonal
    // is not faster than a straight line. The stick alone keeps its length short of full travel,
    // so the game's analog magnitude carries through, and is only brought back from above it.
    bool anyKey = false;
    for (const bool flag : pressed) {
        anyKey = anyKey || flag;
    }
    if (!anyKey && lengthSquared <= kMaximumLengthSquared) {
        return move;
    }
    const float length = std::sqrt(lengthSquared);
    for (float& lane : move) {
        lane /= length;
    }
    return move;
}

/**
 * Shortens a velocity to a speed limit, keeping its direction.
 * @param velocity Velocity to limit in place.
 * @param limit Highest speed to leave.
 */
void cap_speed(teleport::Vector& velocity, float limit) noexcept {
    float speedSquared = 0.0F;
    for (const float lane : velocity) {
        speedSquared += lane * lane;
    }
    if (speedSquared <= limit * limit) {
        return;
    }
    const float scale = limit / std::sqrt(speedSquared);
    for (float& lane : velocity) {
        lane *= scale;
    }
}

/**
 * Writes the horizontal lanes of a body's velocity and leaves the vertical lane as it was.
 * @param body Character rigid body. Live only inside the step or sync hook.
 * @param velocity Source of the two horizontal lanes.
 */
void write_horizontal_velocity(void* body, const teleport::Vector& velocity) noexcept {
    noclip::Vector stored{};
    noclip::read_body_velocity(body, stored);
    stored[kLaneX] = velocity[kLaneX];
    stored[kLaneY] = velocity[kLaneY];
    noclip::write_body_velocity(body, stored);
}

/**
 * Works out the velocity the keys and, optionally, the stick ask for. Also records whether a
 * press owns the vertical lane.
 * @param speed Configured speed.
 * @param withStick True to add the controller's left stick. Fly and movement speed both add it.
 * @return Velocity in world units per second.
 */
[[nodiscard]] teleport::Vector desired_velocity(float speed, bool withStick) noexcept {
    // The interface or another application owns the input. Its presses must not steer.
    std::array<bool, kDirectionCount> pressed{};
    StickMove stick{};
    if (!core::ui::runtime::snapshot().visible && input::game_focused()) {
        pressed = pressed_directions();
        if (withStick) {
            stick = stick_move();
        }
    }
    teleport::Vector forward{};
    const teleport::Vector move = teleport::camera_forward(forward)
                                      ? travel(pressed, stick, forward)
                                      : teleport::Vector{};
    g_steered = move[teleport::kVerticalLane] != 0.0F;
    teleport::Vector velocity{};
    for (std::size_t lane = 0; lane < teleport::kVectorLanes; ++lane) {
        velocity[lane] = move[lane] * speed;
    }
    return velocity;
}

/**
 * Reads the movement speed toggle key once a frame and flips its switch on the press. The same
 * shape as the fly poll below, kept apart so fly's own path stays as it was.
 */
void poll_speed_toggle() noexcept {
    const client::movement::Settings settings = client::movement::get();
    if (settings.movementSpeedToggleKey == client::movement::kNoKey) {
        g_speedToggleDown.store(false, std::memory_order_relaxed);
        return;
    }
    const bool down = input::game_focused()
                      && (GetAsyncKeyState(static_cast<int>(settings.movementSpeedToggleKey))
                          & kKeyHeldBit)
                             != 0;
    // The interface owns the keyboard, so the key tracks the press but never flips the switch.
    if (core::ui::runtime::snapshot().visible) {
        g_speedToggleDown.store(down, std::memory_order_relaxed);
        return;
    }
    if (down && !g_speedToggleDown.exchange(true, std::memory_order_acq_rel)) {
        client::movement::Settings updated = settings;
        updated.movementSpeedEnabled = !settings.movementSpeedEnabled;
        if (!client::movement::publish(updated)) {
            return;
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         updated.movementSpeedEnabled ? "ev=movement_speed stage=toggle enabled=1"
                                                      : "ev=movement_speed stage=toggle enabled=0");
        return;
    }
    if (!down) {
        g_speedToggleDown.store(false, std::memory_order_release);
    }
}

} // namespace

/** Finds the game's processed left-stick move vector. A miss leaves the stick at zero. */
void resolve_controller() noexcept {
    std::byte* const finalize =
        patterns::scan_main_image_unique(kStickFinalize, "movement_stick_finalize");
    if (finalize == nullptr) {
        g_playerContexts.store(nullptr, std::memory_order_release);
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=movement stage=controller result=fail reason=signature");
        return;
    }
    // The array the finalize addresses, decoded from its own `lea`, like the camera singleton.
    g_playerContexts.store(patterns::resolve_relative(finalize + kContextOperandOffset,
                                                      finalize + kContextInstructionEnd),
                           std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=movement stage=controller result=ok");
}

/** Drops the stick source. */
void clear_controller() noexcept {
    g_playerContexts.store(nullptr, std::memory_order_release);
}

/** Reads the fly and movement speed toggle keys once a frame and flips each switch on its press. */
void poll_toggle() noexcept {
    poll_speed_toggle();
    const client::movement::Settings settings = client::movement::get();
    if (settings.flyToggleKey == client::movement::kNoKey) {
        g_toggleDown.store(false, std::memory_order_relaxed);
        return;
    }
    const bool down =
        input::game_focused()
        && (GetAsyncKeyState(static_cast<int>(settings.flyToggleKey)) & kKeyHeldBit) != 0;
    // The interface owns the keyboard, so the key tracks the press but never flips the switch.
    if (core::ui::runtime::snapshot().visible) {
        g_toggleDown.store(down, std::memory_order_relaxed);
        return;
    }
    if (down && !g_toggleDown.exchange(true, std::memory_order_acq_rel)) {
        client::movement::Settings updated = settings;
        updated.flyEnabled = !settings.flyEnabled;
        if (!client::movement::publish(updated)) {
            return;
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         updated.flyEnabled ? "ev=fly stage=toggle enabled=1"
                                            : "ev=fly stage=toggle enabled=0");
        return;
    }
    if (!down) {
        g_toggleDown.store(false, std::memory_order_release);
    }
}

/** Writes the player's velocity on the physics sync, which publishes it. */
void apply(void* component) noexcept {
    const client::movement::Settings settings = client::movement::get();
    if (!settings.flyEnabled) {
        return;
    }
    if (component == nullptr || !teleport::owns_local_player(component)) {
        return;
    }
    if (!read_bindings()) {
        return;
    }
    // Capped, because this is the field the game reads to decide the player hit something too
    // hard. The step has the real speed; this is only what the sync publishes.
    teleport::Vector velocity = desired_velocity(settings.flySpeed, true);
    cap_speed(velocity, kPublishedSpeedCap);
    (void)teleport::write_velocity(component, velocity);
}

/** Reports whether fly is on. */
bool enabled() noexcept {
    return client::movement::get().flyEnabled;
}

/** Sets the velocity the coming simulation step integrates. */
void before_step(void* body) noexcept {
    g_heightValid = false;
    if (body == nullptr || !read_bindings()) {
        return;
    }
    noclip::write_body_velocity(body, desired_velocity(client::movement::get().flySpeed, true));
    noclip::Vector position{};
    noclip::read_body_position(body, position);
    g_heightBeforeStep = position[teleport::kVerticalLane];
    g_heightValid = true;
}

/** Puts back the height the step's gravity took. */
void after_step(void* body, bool heldElsewhere) noexcept {
    if (body == nullptr || heldElsewhere || g_steered || !g_heightValid) {
        return;
    }
    // The height from before this step, so a respawn or any other placement is kept.
    noclip::Vector position{};
    noclip::read_body_position(body, position);
    position[teleport::kVerticalLane] = g_heightBeforeStep;
    noclip::write_body_position(body, position);
    // The step also left its gravity in the velocity, where it would build up.
    noclip::Vector velocity{};
    noclip::read_body_velocity(body, velocity);
    velocity[teleport::kVerticalLane] = 0.0F;
    noclip::write_body_velocity(body, velocity);
}

/** Reports whether movement speed drives the horizontal lanes. Fly takes precedence. */
bool speed_enabled() noexcept {
    const client::movement::Settings settings = client::movement::get();
    return settings.movementSpeedEnabled && !settings.flyEnabled;
}

/** Replaces the horizontal lanes for the coming step while the game moves the player itself. */
bool before_speed_step(void* body) noexcept {
    // A step whose body was replaced never reached its restore; those lanes belonged to that body.
    g_speedNative = NativeHorizontal{};
    if (body == nullptr || !read_bindings()) {
        return false;
    }
    const teleport::Vector desired = desired_velocity(client::movement::get().movementSpeed, true);
    noclip::Vector native{};
    noclip::read_body_velocity(body, native);
    const float desiredSquared =
        desired[kLaneX] * desired[kLaneX] + desired[kLaneY] * desired[kLaneY];
    const float nativeSquared = native[kLaneX] * native[kLaneX] + native[kLaneY] * native[kLaneY];
    // Without a move asked for, the game's lanes stay as they are: knockback, pushes and its own
    // stop all come through.
    if (desiredSquared <= kMinimumLengthSquared) {
        return false;
    }
    // The keys and the stick are read raw. The game decides whether they move the player, and an
    // interface that took them leaves its lanes at rest, so the raised speed follows that decision.
    if (nativeSquared <= kNativeStationarySquared) {
        return false;
    }
    g_speedNative = NativeHorizontal{body, native[kLaneX], native[kLaneY]};
    write_horizontal_velocity(body, desired);
    return true;
}

/** Puts the game's own horizontal lanes back after a step movement speed raised. */
void after_speed_step(void* body) noexcept {
    const NativeHorizontal native = g_speedNative;
    g_speedNative = NativeHorizontal{};
    // Nothing was raised this step, or the lanes belong to another body.
    if (native.body == nullptr || native.body != body) {
        return;
    }
    // The step has already moved the player at the raised speed. The body leaves it with the
    // game's own lanes, so the next tick starts from what the game made, and the test above reads
    // the game alone. The vertical lane is the step's result and stays as it is.
    write_horizontal_velocity(body, teleport::Vector{native.x, native.y, 0.0F});
}

/** Clears the key state, the held height and the held lanes. The switches are stored settings. */
void reset() noexcept {
    g_toggleDown.store(false, std::memory_order_release);
    g_speedToggleDown.store(false, std::memory_order_release);
    g_heightValid = false;
    g_speedNative = NativeHorizontal{};
}

} // namespace sunrise::client::hooks::fly

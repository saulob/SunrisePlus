#pragma once

namespace sunrise::client::hooks::fly {

/**
 * Fastest speed the game is shown while this hook drives the position itself. Contact with
 * geometry damages the player above roughly this, and the real speed is not needed for movement.
 */
inline constexpr float kPublishedSpeedCap = 8.0F;

/**
 * Finds the game's processed left-stick move vector, which is read every tick alongside the keys.
 * A miss is logged and leaves the stick contribution at zero; fly and movement speed still move
 * from the keys.
 */
void resolve_controller() noexcept;

/** Drops the stick source. The next tick uses the keys alone. */
void clear_controller() noexcept;

/** Reads the fly and movement speed toggle keys once a frame and flips each switch on its press. */
void poll_toggle() noexcept;

/**
 * Writes the player's velocity on the physics sync, which publishes it.
 * @param component Physics component being synced. Tested for player ownership here.
 */
void apply(void* component) noexcept;

/** @return True while fly is on. */
[[nodiscard]] bool enabled() noexcept;

/**
 * Sets the velocity the coming simulation step integrates. Noclip reads it too.
 * @param body Character rigid body. Live only inside the step hook.
 */
void before_step(void* body) noexcept;

/**
 * Puts back the height the step's gravity took.
 * @param body Character rigid body. Live only inside the step hook.
 * @param heldElsewhere True when noclip carries the vertical lane.
 */
void after_step(void* body, bool heldElsewhere) noexcept;

/**
 * Movement speed: the same keys, left stick and camera-relative direction as fly, at a configured
 * speed, on the horizontal lanes only. The vertical lane stays the game's,
 * so gravity, jumping and falling are untouched. It only raises a horizontal move the game is
 * already making, and only for the step it is integrated in.
 * @return True while movement speed may drive the horizontal lanes: on, with fly off. Noclip may
 * be on; it then carries this speed through geometry.
 */
[[nodiscard]] bool speed_enabled() noexcept;

/**
 * Replaces the horizontal lanes for the coming simulation step with the configured speed, only
 * while the keys or the stick ask for a move and the game is already moving the player
 * horizontally. Otherwise the game's lanes are left as they are.
 * @param body Character rigid body. Live only inside the step hook.
 * @return True when the lanes were replaced. The game's own are put back after the step.
 */
[[nodiscard]] bool before_speed_step(void* body) noexcept;

/**
 * Puts back the game's own horizontal lanes after a step movement speed raised, so the raised
 * speed never carries into the next tick. Does nothing after a step it left alone.
 * @param body Character rigid body. Live only inside the step hook.
 */
void after_speed_step(void* body) noexcept;

/** Clears the key state and the held lanes. The switches are stored settings and survive. */
void reset() noexcept;

} // namespace sunrise::client::hooks::fly

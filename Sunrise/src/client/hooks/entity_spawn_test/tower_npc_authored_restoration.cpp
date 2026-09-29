#include "tower_npc_authored_restoration.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include "../../../core/logging/log.h"
#include "../../../middleware/bap/activity_message/squad_auth_body.h"
#include "../../../server/activity/activity_sdk_behavior_scope.h"
#include "../../../server/activity/activity_sdk_mission_internal.h"
#include "../../../server/activity/activity_sdk_mission_runtime.h"
#include "../../../server/activity/activity_sdk_squad_runtime.h"
#include "../../../server/activity/host_runtime.h"
#include "../../../server/activity/mission/mission_script_runtime.h"
#include "../../../server/bap/runtime.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/activity/runtime.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../world_objects/world_object_registry.h"

namespace sunrise::client::hooks::entity_spawn_test::tower_npc_authored_restoration {
namespace {

namespace activity = ::sunrise::state::activity;
namespace bap = ::sunrise::server::bap;
namespace behavior_scope = ::sunrise::server::activity::behavior_scope;
namespace format = ::sunrise::state::activity_sdk::format;
namespace host = ::sunrise::server::activity::host;
namespace mission = ::sunrise::server::activity::activity_sdk_mission;
namespace mission_detail = ::sunrise::server::activity::activity_sdk_mission::detail;
namespace script = ::sunrise::server::activity::mission;
namespace sdk = ::sunrise::state::activity_sdk;
namespace squad_auth = ::sunrise::middleware::bap::activity_message::squad_auth;
namespace squads = ::sunrise::server::activity::activity_sdk_squads;

constexpr std::string_view kTowerActivity = "city_tower_social_d2";
constexpr std::uint32_t kAbsent = format::kAbsentIndex;
constexpr std::uint32_t kNoHandle = 0xFFFFFFFFU;
/** Distinct state names one sensor can offer; the World page uses the same capacity. */
constexpr std::size_t kStateCapacity = 160;
/** Performance sensors per encounter; Xur's encounter carries several. */
constexpr std::size_t kSensorCapacity = 32;
/** Live copies the allocation tracer keeps per NPC. */
constexpr std::size_t kCopyCapacity = 8;
/** The native actor showed up about 94 ms after the place in every test; 2 s is the limit. */
constexpr std::uint64_t kDetectMs = 2000;
/** Checks while the squad is not placeable or the Tower script still holds the host. */
constexpr std::uint64_t kAvailabilityRetryMs = 250;
/** Read-only readiness checks while the actor waits for its state. */
constexpr std::uint64_t kReadinessPollMs = 500;
/** Next readiness check after a state request was refused although the checks passed. */
constexpr std::uint64_t kPerformanceBackoffMs = 1000;
/** A vanished actor's next cycle starts no sooner than this. */
constexpr std::uint64_t kRearmDelayMs = 1000;
/**
 * The Tower mission script places a bubble's squads, objects, scenes and idles in one burst on
 * its first visit, one intent at a time, and drops an intent the host cannot reserve for: in
 * npc42 our Yuna place cost Hawthorne's (host_reservation_unavailable 62 ms later) and our Xur
 * place cost Amanda's. Its first intent follows the bubble change by about 60 ms, so none of our
 * requests goes out in the first kScriptHeadStartMs of a visit, and none while the script still
 * has intents in flight, up to kScriptYieldMs after entering the bubble.
 */
constexpr std::uint64_t kScriptHeadStartMs = 500;
constexpr std::uint64_t kScriptYieldMs = 3000;
/**
 * Creation flags of an authored squad actor (npc28, npc29, npc36); the factory and the game's
 * native_create_entity copies carry 0x300 and no squad sensor.
 */
constexpr std::uint32_t kAuthoredFlags = 0x380U;

using Counts = std::array<std::int32_t, squad_auth::kMaximumRequestedCountLength>;

/**
 * One NPC's authored squad, as the squad tests confirmed it in runtime. `registryKey` is the
 * encounter object's key in the SDK catalog; it only identifies the row and is never published.
 */
struct AuthoredNpc {
    const char* name;
    std::uint32_t entity;
    std::uint32_t encounter;
    std::uint32_t registryKey;
    std::int32_t bubble;
    std::uint32_t squadRow;
    std::string_view squadSlot;
    std::uint32_t sensorRow;
    std::string_view sensor;
    std::uint32_t state;
};
constexpr std::array kAuthored{
    AuthoredNpc{"Yuna", 0x80C93820U, 0x80B4A54AU, 0x2EFB59ADU, 1, 46242U, "sq_vendor_igr",
                14805U, "sq_vendor_igr_idle", 0xC7B78475U},
    AuthoredNpc{"Saladin", 0x80BC8E4BU, 0x80B4A5CDU, 0x27060E6CU, 6, 46314U,
                "sq_vendor_iron_banner", 14823U, "sq_vendor_iron_banner_idle", 0x6E956079U},
    AuthoredNpc{"Xur", 0x80BDEBBFU, 0x80B4AD29U, 0x728E75D1U, 7, 46321U, "xur.xur_squad",
                15585U, "xur.xur_squad_idle", 0x6A49E4BCU},
};
constexpr std::size_t kAuthoredCount = kAuthored.size();

/**
 * Where one NPC's cycle stands in the current residence. Only an accepted place leaves `idle`
 * for a new actor; a refusal before the place is queued checks again after the interval.
 */
enum class Phase : std::uint8_t {
    idle,
    /** squads::place was accepted; waiting at most kDetectMs for its native actor. */
    awaitingNative,
    /** The authored actor is live (detected or adopted); its state waits for readiness. */
    waitingPerformance,
    /** Its state was accepted. */
    restored,
    /** The accepted place produced no native actor in time. */
    failed,
};

/** One NPC's restoration cycle. Only the game-thread service touches it. */
struct Cycle {
    Phase phase{Phase::idle};
    bool inBubble{};
    std::uint32_t handle{kNoHandle};
    /** Native copies live before the place, so only a new one counts as its result. */
    std::array<std::uint32_t, kCopyCapacity> baseline{};
    std::size_t baselineCount{};
    std::uint64_t placedAt{};
    std::uint64_t nextAttempt{};
    /** Last skip reason logged, so a repeated skip logs once. */
    const char* lastSkip{};
    /** Keys of the last availability, place and waiting lines, so a repeat logs once. */
    std::uint32_t lastAvailability{};
    std::uint32_t lastPlace{};
    std::uint32_t lastWaiting{};
};
std::array<Cycle, kAuthoredCount> g_cycles{};
/** Tower session the cycles belong to; a new one forgets them. */
std::uint64_t g_session{activity::kAbsentSessionId};
/** Runtime toggles, set from the UI thread. Every NPC starts enabled. */
std::array<std::atomic_bool, kAuthoredCount> g_enabled{true, true, true};
/** Set by an off-to-on toggle; the game thread clears a failed cycle for it. */
std::array<std::atomic_bool, kAuthoredCount> g_recover{};

/** Game-owned copies already reported this session, so each is reported once. */
struct Observed {
    std::uint32_t conditional{kNoHandle};
    std::uint32_t duplicateConditional{kNoHandle};
    std::uint32_t duplicateAuthored{kNoHandle};
};
std::array<Observed, kAuthoredCount> g_observed{};

// Skip reasons, compared by pointer so a repeated skip logs once.
constexpr const char* kSkipDisabled = "disabled";
constexpr const char* kSkipAlreadyNative = "already_native";
constexpr const char* kSkipAlreadyRestored = "already_restored";
constexpr const char* kSkipCycleFailed = "cycle_failed";
constexpr const char* kSkipWrongBubble = "wrong_bubble";

void log_authored(const AuthoredNpc& npc, const char* stage, const char* detail) noexcept {
    core::log::writef(core::log::Channel::client, core::log::Level::warn,
                      "DEBUG_SAULO tower_npc_authored name=%s stage=%s %s", npc.name, stage,
                      detail);
}

/** Logs a skip only when its reason changes. */
void note_skip(std::size_t index, const char* reason, const char* detail) noexcept {
    Cycle& cycle = g_cycles[index];
    if (cycle.lastSkip == reason) {
        return;
    }
    cycle.lastSkip = reason;
    std::array<char, 224> line{};
    (void)std::snprintf(line.data(), line.size(), "reason=%s %s", reason, detail);
    log_authored(kAuthored[index], "skip", line.data());
}

/** @return FNV-1a of one line, never zero, so a repeated line can be told apart. */
[[nodiscard]] std::uint32_t line_key(std::string_view text) noexcept {
    std::uint32_t value = 0x811C'9DC5U;
    for (const char character : text) {
        value ^= static_cast<unsigned char>(character);
        value *= 0x0100'0193U;
    }
    return value == 0 ? 1U : value;
}

/** Logs one stage line only when it differs from the last one `last` remembers. */
void note_once(std::size_t index, std::uint32_t& last, const char* stage,
               const char* detail) noexcept {
    const std::uint32_t key = line_key(detail);
    if (last == key) {
        return;
    }
    last = key;
    log_authored(kAuthored[index], stage, detail);
}

[[nodiscard]] bool none(const char* reason) noexcept {
    return std::strcmp(reason, "none") == 0;
}

/** Bubble the service saw last, and when the player entered it. */
std::int32_t g_bubble{-1};
std::uint64_t g_bubbleSince{};
/** Game-thread copy of the mission script diagnostics; too large for the update's stack. */
script::DiagnosticsSnapshot g_script{};

/** @return True while our requests should leave the host to the Tower script's first burst. */
[[nodiscard]] bool yield_to_script(std::uint64_t now) noexcept {
    const std::uint64_t visit = now - g_bubbleSince;
    if (visit < kScriptHeadStartMs) {
        return true;
    }
    if (visit >= kScriptYieldMs) {
        return false;
    }
    const std::uint64_t session =
        activity::membership::live_region_session(activity::kAbsentSessionId);
    activity::SessionBinding binding{};
    if (session == activity::kAbsentSessionId || !activity::snapshot_binding(session, binding)) {
        return false;
    }
    script::snapshot(g_script);
    for (std::size_t index = 0; index < g_script.instanceCount && index < g_script.instances.size();
         ++index) {
        const script::InstanceDiagnostics& instance = g_script.instances[index];
        if (activity::same_binding(instance.binding, binding)) {
            return instance.pendingIntents != 0
                   || std::string_view(instance.deliveryStage.data()) != "idle";
        }
    }
    return false;
}

/** The live Tower session, bound the way Activity Host -> World binds its selected instance. */
struct Bound {
    sdk::BoundView view{};
    host::InstanceSnapshot instance{};
};

/** @return "none" once bound, otherwise why not. */
[[nodiscard]] const char* bind_tower(Bound& output) noexcept {
    output = {};
    const std::uint64_t session =
        activity::membership::live_region_session(activity::kAbsentSessionId);
    activity::SessionBinding binding{};
    if (session == activity::kAbsentSessionId || !activity::snapshot_binding(session, binding)) {
        return "no_session";
    }
    const auto& destination = binding.destination;
    if (destination.packageNameLength != kTowerActivity.size()
        || std::memcmp(destination.packageName.data(), kTowerActivity.data(),
                       kTowerActivity.size()) != 0) {
        return "not_in_tower";
    }
    if (!host::instance_snapshot(binding, output.instance)) {
        return "host_instance_missing";
    }
    sdk::Snapshot catalog = sdk::snapshot();
    if (!catalog) {
        return "sdk_catalog_unavailable";
    }
    // World's bind: resolve against the current link, then revalidate against a fresh read.
    bap::ActivityLinkView link{};
    (void)bap::activity_link_view(output.instance.binding, link);
    const sdk::Selection selection{
        output.instance.binding, link.matchingLinks, link.activityClientGeneration};
    sdk::Status status = sdk::resolve(std::move(catalog), selection, output.view);
    if (status == sdk::Status::ready) {
        bap::ActivityLinkView current{};
        (void)bap::activity_link_view(output.instance.binding, current);
        status = sdk::revalidate(output.view, output.instance.binding, current.matchingLinks,
                                 current.activityClientGeneration);
    }
    return status == sdk::Status::ready ? "none" : sdk::status_name(status);
}

/** @return A slot's catalog name, never empty, so a `%.*s` never reads a null pointer. */
[[nodiscard]] std::string_view slot_name(const sdk::Catalog& catalog,
                                         std::uint32_t slotRow) noexcept {
    if (slotRow >= catalog.slots().size()) {
        return "none";
    }
    const std::string_view name = catalog.string(catalog.slots()[slotRow].name);
    return name.empty() ? std::string_view{"unnamed"} : name;
}

/** World's Idles filter: an exact type-42 slot. */
[[nodiscard]] bool exact_performance_slot(const format::Slot& slot) noexcept {
    return slot.slotType == format::kPerformanceSlotType
           && slot.authSchema == format::kPerformanceAuthSchema
           && (slot.flags & format::kSlotSchemaJoinExact) != 0;
}

/**
 * Lists the distinct exact type-42 slots on the NPC's encounter object, walking the bound
 * scenario's occurrences as the World page does.
 * @param occurrences Receives how many occurrences of the encounter the scenario has.
 */
[[nodiscard]] std::size_t encounter_sensors(const sdk::BoundView& view, const AuthoredNpc& npc,
                                            std::span<std::uint32_t> output,
                                            std::size_t& occurrences) noexcept {
    occurrences = 0;
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    if (scenario == nullptr) {
        return 0;
    }
    const sdk::Catalog& catalog = *view.catalog;
    const auto objects = catalog.objects();
    const auto slots = catalog.slots();
    std::size_t written = 0;
    for (const format::Occurrence& occurrence : sdk::scenario_occurrences(catalog, *scenario)) {
        if (occurrence.objectIndex >= objects.size()) {
            continue;
        }
        const format::Object& object = objects[occurrence.objectIndex];
        if (object.objectTag != npc.encounter || object.objectKey != npc.registryKey) {
            continue;
        }
        ++occurrences;
        for (const format::Slot& slot : sdk::object_slots(catalog, object)) {
            const auto row = static_cast<std::uint32_t>(&slot - slots.data());
            const auto end = output.begin() + static_cast<std::ptrdiff_t>(written);
            if (exact_performance_slot(slot) && written < output.size()
                && std::find(output.begin(), end, row) == end) {
                output[written++] = row;
            }
        }
    }
    return written;
}

/** A sensor drives the one squad slot its exact performance-target edge names. */
[[nodiscard]] std::uint32_t sensor_squad_slot(const sdk::Catalog& catalog,
                                              const format::Slot& slot) noexcept {
    std::uint32_t squadSlot = kAbsent;
    std::size_t edges = 0;
    for (const format::AuthoredSceneSquadEdge& edge :
         sdk::slot_authored_scene_squad_edges(catalog, slot)) {
        if ((edge.flags & format::kAuthoredSceneSquadPerformanceTargetExact) != 0) {
            squadSlot = edge.squadSlotIndex;
            ++edges;
        }
    }
    return edges == 1 ? squadSlot : kAbsent;
}

/** The Activity Host Squads page defaults: each member's authored count, never below zero. */
[[nodiscard]] std::size_t default_counts(const sdk::Catalog& catalog, const format::Squad& squad,
                                         std::span<std::int32_t> output) noexcept {
    std::size_t count = 0;
    for (const format::SquadMember& member : sdk::squad_members(catalog, squad)) {
        if (count == output.size()) {
            break;
        }
        output[count++] = (std::max)(member.defaultCount, 0);
    }
    return count;
}

/** The NPC's live copies the allocation tracer recorded, split by origin. */
struct Copies {
    std::array<world_objects::NpcCopy, kCopyCapacity> native{};
    std::size_t nativeCount{};
    /** First native_create_entity copy: the game's own conditional copy, never an authored one. */
    std::uint32_t conditional{kNoHandle};
    std::uint32_t conditionalFlags{};
};

[[nodiscard]] Copies read_copies(std::uint32_t entity) noexcept {
    Copies result{};
    if (!world_objects::is_installed()) {
        return result;
    }
    std::array<world_objects::NpcCopy, kCopyCapacity> copies{};
    const std::size_t count = world_objects::live_npc_copies(entity, copies);
    for (std::size_t index = 0; index < count; ++index) {
        const world_objects::NpcCopy& copy = copies[index];
        if (copy.origin == nullptr) {
            continue;
        }
        if (std::strcmp(copy.origin, "native_instantiate") == 0) {
            if (result.nativeCount < result.native.size()) {
                result.native[result.nativeCount++] = copy;
            }
        } else if (std::strcmp(copy.origin, "native_create_entity") == 0
                   && result.conditional == kNoHandle) {
            result.conditional = copy.handle;
            result.conditionalFlags = copy.flags;
        }
    }
    return result;
}

/** An authored squad actor: native_instantiate, squad-path flags and the squad sensor. */
[[nodiscard]] bool authored_shape(const world_objects::NpcCopy& copy) noexcept {
    return copy.flags == kAuthoredFlags && copy.squadSensor;
}

/** @return "none" when the confirmed squad row still names the NPC's squad slot. */
[[nodiscard]] const char* check_squad(const sdk::BoundView& view, const AuthoredNpc& npc,
                                      std::uint32_t& squadSlot) noexcept {
    squadSlot = kAbsent;
    const sdk::Catalog& catalog = *view.catalog;
    const auto table = catalog.squads();
    if (npc.squadRow >= table.size()) {
        return "squad_row_out_of_range";
    }
    const format::Squad& squad = table[npc.squadRow];
    const auto objects = catalog.objects();
    if (squad.objectIndex >= objects.size()
        || objects[squad.objectIndex].objectTag != npc.encounter
        || objects[squad.objectIndex].objectKey != npc.registryKey) {
        return "squad_row_other_object";
    }
    if (slot_name(catalog, squad.slotIndex) != npc.squadSlot) {
        return "squad_row_other_slot";
    }
    squadSlot = squad.slotIndex;
    return "none";
}

/**
 * Finds the idle sensor by name on the encounter, as the World page lists it, and checks it is
 * the confirmed row and drives the squad slot.
 * @return "none" when found.
 */
[[nodiscard]] const char* resolve_sensor(const sdk::BoundView& view, const AuthoredNpc& npc,
                                         std::uint32_t squadSlot,
                                         std::uint32_t& sensorRow) noexcept {
    sensorRow = kAbsent;
    const sdk::Catalog& catalog = *view.catalog;
    std::array<std::uint32_t, kSensorCapacity> sensors{};
    std::size_t occurrences = 0;
    const std::size_t count = encounter_sensors(view, npc, sensors, occurrences);
    std::size_t matches = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (slot_name(catalog, sensors[index]) == npc.sensor) {
            sensorRow = sensors[index];
            ++matches;
        }
    }
    if (occurrences == 0) {
        return "encounter_absent";
    }
    if (matches != 1) {
        return matches == 0 ? "sensor_absent" : "sensor_ambiguous";
    }
    if (sensorRow != npc.sensorRow) {
        return "sensor_row_mismatch";
    }
    if (sensor_squad_slot(catalog, catalog.slots()[sensorRow]) != squadSlot) {
        return "sensor_targets_other_squad";
    }
    return "none";
}

/** @return True when the sensor's actor declares the state, as the World page offers it. */
[[nodiscard]] bool state_declared(const sdk::BoundView& view, std::uint32_t sensorRow,
                                  std::uint32_t state) noexcept {
    std::array<std::uint32_t, kStateCapacity> names{};
    const std::size_t count = sdk::performance_state_names(
        *view.catalog, *sdk::bound_scenario(view), sensorRow, names);
    const auto end = names.begin() + static_cast<std::ptrdiff_t>(count);
    return std::find(names.begin(), end, state) != end;
}

/** What play_performance_slot's own checks answer for one slot, before anything is queued. */
struct DryRun {
    mission::SceneStatus status{mission::SceneStatus::invalidView};
    mission_detail::PreparedScene prepared{};
};
/** Game-thread storage; the prepared scene is too large for the update's stack. */
DryRun g_dryRun{};

/**
 * play_performance_slot's checks in its order: the slot's current behavior occurrence (mission
 * query, scene binding, live-state selection), then the typed-behavior preparation, which holds
 * the scene lease (mission seed published) and the route. Nothing changes state.
 */
void performance_dry_run(const sdk::BoundView& view, std::uint32_t slotRow,
                         DryRun& output) noexcept {
    output.status = mission::SceneStatus::invalidView;
    mission::Snapshot snapshot{};
    if (mission::query(view, snapshot) != mission::Status::ready || view.catalog == nullptr) {
        return;
    }
    const auto slots = view.catalog->slots();
    if (slotRow >= slots.size()) {
        output.status = mission::SceneStatus::invalidSlot;
        return;
    }
    bap::ActivityLinkView link{};
    output.status = mission_detail::scene_binding_status(view, link);
    if (output.status != mission::SceneStatus::ready) {
        return;
    }
    const auto selected = behavior_scope::select(view.catalog->occurrences(),
                                                 view.catalog->states(),
                                                 view.catalog->bubbles(),
                                                 view.scenarioRow,
                                                 slots[slotRow].objectIndex,
                                                 snapshot.plan.stateRow,
                                                 link.effectiveRegion);
    if (selected.ambiguous) {
        output.status = mission::SceneStatus::ambiguousTarget;
        return;
    }
    if (selected.row == kAbsent) {
        output.status = mission::SceneStatus::targetUnavailable;
        return;
    }
    output.status = mission_detail::prepare_typed_behavior(view,
                                                           selected.row,
                                                           slotRow,
                                                           format::kPerformanceSlotType,
                                                           format::kPerformanceComponentClass,
                                                           format::kPerformanceAuthSchema,
                                                           false,
                                                           output.prepared);
}

/** @return "none" when the host would take a new request now, otherwise why not. */
[[nodiscard]] const char* host_lane(const host::InstanceSnapshot& instance) noexcept {
    return !instance.active                        ? "host_inactive"
           : instance.outputPending                ? "host_output_pending"
           : instance.scriptableReservationPending ? "host_reservation_pending"
                                                   : "none";
}

/**
 * Read-only readiness of the NPC's state request: binding, squad and sensor rows, declared
 * state, play_performance_slot's own checks, then the host lane its enqueue meets last.
 * @return "none" when the request would be accepted now, otherwise why not yet.
 */
[[nodiscard]] const char* performance_readiness(const AuthoredNpc& npc, Bound& bound,
                                                std::uint32_t& sensorRow) noexcept {
    sensorRow = kAbsent;
    const char* reason = bind_tower(bound);
    std::uint32_t squadSlot = kAbsent;
    if (none(reason)) {
        reason = check_squad(bound.view, npc, squadSlot);
    }
    if (none(reason)) {
        reason = resolve_sensor(bound.view, npc, squadSlot, sensorRow);
    }
    if (none(reason) && !state_declared(bound.view, sensorRow, npc.state)) {
        reason = "state_undeclared";
    }
    if (!none(reason)) {
        return reason;
    }
    performance_dry_run(bound.view, sensorRow, g_dryRun);
    if (g_dryRun.status != mission::SceneStatus::ready) {
        return mission::status_name(g_dryRun.status);
    }
    return host_lane(bound.instance);
}

/**
 * Waits for readiness, then plays the state once through the World page's play_performance_slot.
 * A refusal only waits again: the actor exists, so the squad is never placed for it again.
 */
void poll_performance(std::size_t index, std::uint64_t now) noexcept {
    const AuthoredNpc& npc = kAuthored[index];
    Cycle& cycle = g_cycles[index];
    if (now < cycle.nextAttempt) {
        return;
    }
    std::array<char, 192> line{};
    if (yield_to_script(now)) {
        cycle.nextAttempt = now + kAvailabilityRetryMs;
        (void)std::snprintf(line.data(), line.size(), "handle=0x%08X reason=script_busy",
                            cycle.handle);
        note_once(index, cycle.lastWaiting, "waiting_performance", line.data());
        return;
    }
    Bound bound{};
    std::uint32_t sensorRow = kAbsent;
    const char* const reason = performance_readiness(npc, bound, sensorRow);
    if (!none(reason)) {
        cycle.nextAttempt = now + kReadinessPollMs;
        (void)std::snprintf(line.data(), line.size(), "handle=0x%08X reason=%s", cycle.handle,
                            reason);
        note_once(index, cycle.lastWaiting, "waiting_performance", line.data());
        return;
    }
    (void)std::snprintf(line.data(), line.size(), "handle=0x%08X sensor_row=%u", cycle.handle,
                        sensorRow);
    log_authored(npc, "performance_ready", line.data());
    const mission::SceneStatus status =
        mission::play_performance_slot(bound.view, sensorRow, npc.state);
    (void)std::snprintf(line.data(), line.size(),
                        "state=0x%08X handle=0x%08X sensor_row=%u result=%s", npc.state,
                        cycle.handle, sensorRow, mission::status_name(status));
    log_authored(npc, "performance", line.data());
    if (status == mission::SceneStatus::queued) {
        cycle.phase = Phase::restored;
        (void)std::snprintf(line.data(), line.size(), "result=ok handle=0x%08X", cycle.handle);
        log_authored(npc, "complete", line.data());
        return;
    }
    cycle.nextAttempt = now + kPerformanceBackoffMs;
    (void)std::snprintf(line.data(), line.size(), "handle=0x%08X reason=performance_%s",
                        cycle.handle, mission::status_name(status));
    note_once(index, cycle.lastWaiting, "waiting_performance", line.data());
}

/** Starts waiting for readiness with a live authored actor; a ready world plays at once. */
void enter_waiting(std::size_t index, std::uint32_t handle, const char* source,
                   std::uint64_t now) noexcept {
    Cycle& cycle = g_cycles[index];
    cycle.phase = Phase::waitingPerformance;
    cycle.handle = handle;
    cycle.nextAttempt = now;
    cycle.lastSkip = nullptr;
    std::array<char, 96> line{};
    (void)std::snprintf(line.data(), line.size(), "handle=0x%08X reason=entered source=%s",
                        handle, source);
    cycle.lastWaiting = line_key(line.data());
    log_authored(kAuthored[index], "waiting_performance", line.data());
    poll_performance(index, now);
}

/**
 * Availability, then the one squads::place this cycle makes, with the Activity Host Squads page
 * defaults (authored counts, mode 0). Anything short of an accepted place leaves the cycle idle
 * for the next check.
 */
void place_squad(std::size_t index, const Copies& copies, std::uint64_t now) noexcept {
    const AuthoredNpc& npc = kAuthored[index];
    Cycle& cycle = g_cycles[index];
    std::array<char, 192> line{};
    if (yield_to_script(now)) {
        (void)std::snprintf(line.data(), line.size(), "row=%u result=script_busy next_ms=%llu",
                            npc.squadRow, static_cast<unsigned long long>(kAvailabilityRetryMs));
        note_once(index, cycle.lastAvailability, "availability", line.data());
        return;
    }
    Bound bound{};
    const char* reason = bind_tower(bound);
    std::uint32_t squadSlot = kAbsent;
    if (none(reason)) {
        reason = check_squad(bound.view, npc, squadSlot);
    }
    if (!none(reason)) {
        (void)std::snprintf(line.data(), line.size(),
                            "row=%u result=refused reason=%s next_ms=%llu", npc.squadRow, reason,
                            static_cast<unsigned long long>(kAvailabilityRetryMs));
        note_once(index, cycle.lastAvailability, "availability", line.data());
        return;
    }
    Counts counts{};
    const std::size_t count =
        default_counts(*bound.view.catalog, bound.view.catalog->squads()[npc.squadRow], counts);
    const std::span<const std::int32_t> requested = std::span(counts).first(count);
    const squads::Status available =
        squads::availability(bound.view, npc.squadRow, requested, squad_auth::Mode::mode0);
    if (available != squads::Status::ready) {
        (void)std::snprintf(line.data(), line.size(), "row=%u result=%s next_ms=%llu",
                            npc.squadRow, squads::status_name(available),
                            static_cast<unsigned long long>(kAvailabilityRetryMs));
        note_once(index, cycle.lastAvailability, "availability", line.data());
        return;
    }
    (void)std::snprintf(line.data(), line.size(), "row=%u squad=%.*s result=ready",
                        npc.squadRow, static_cast<int>(npc.squadSlot.size()),
                        npc.squadSlot.data());
    note_once(index, cycle.lastAvailability, "availability", line.data());

    cycle.baselineCount = copies.nativeCount;
    for (std::size_t copy = 0; copy < copies.nativeCount; ++copy) {
        cycle.baseline[copy] = copies.native[copy].handle;
    }
    // The one placement this cycle makes; a refusal placed nothing and waits for the next check.
    const squads::Status placed =
        squads::place(bound.view, npc.squadRow, requested, squad_auth::Mode::mode0);
    if (placed != squads::Status::queued) {
        (void)std::snprintf(line.data(), line.size(), "row=%u result=%s next_ms=%llu",
                            npc.squadRow, squads::status_name(placed),
                            static_cast<unsigned long long>(kAvailabilityRetryMs));
        note_once(index, cycle.lastPlace, "place", line.data());
        return;
    }
    (void)std::snprintf(line.data(), line.size(), "row=%u result=queued", npc.squadRow);
    log_authored(npc, "place", line.data());
    cycle.phase = Phase::awaitingNative;
    cycle.placedAt = now;
    cycle.lastSkip = nullptr;
    cycle.lastAvailability = 0;
    cycle.lastPlace = 0;
}

/** The first native copy absent from the baseline is the placement's result. */
void detect_native(std::size_t index, std::span<const world_objects::NpcCopy> natives,
                   std::uint64_t now) noexcept {
    const AuthoredNpc& npc = kAuthored[index];
    Cycle& cycle = g_cycles[index];
    const auto baselineEnd =
        cycle.baseline.begin() + static_cast<std::ptrdiff_t>(cycle.baselineCount);
    std::array<char, 192> line{};
    for (const world_objects::NpcCopy& copy : natives) {
        if (std::find(cycle.baseline.begin(), baselineEnd, copy.handle) != baselineEnd) {
            continue;
        }
        (void)std::snprintf(line.data(), line.size(),
                            "handle=0x%08X flags=0x%X aux_relative=%lld squad_sensor=%u "
                            "after_ms=%llu",
                            copy.handle, copy.flags, static_cast<long long>(copy.auxRelative),
                            copy.squadSensor ? 1U : 0U,
                            static_cast<unsigned long long>(now - cycle.placedAt));
        log_authored(npc, "native_detected", line.data());
        enter_waiting(index, copy.handle, "detected", now);
        return;
    }
    if (now - cycle.placedAt >= kDetectMs) {
        // The place was accepted, so this residence ends here rather than risk a second squad.
        (void)std::snprintf(line.data(), line.size(),
                            "reason=native_not_detected after_ms=%llu baseline_native=%zu",
                            static_cast<unsigned long long>(now - cycle.placedAt),
                            cycle.baselineCount);
        log_authored(npc, "timeout", line.data());
        cycle.phase = Phase::failed;
    }
}

/**
 * Diagnostic only: reports, once each, the game's own native_create_entity copy and its
 * coexistence with a native_instantiate copy. Neither is ever destroyed or suppressed.
 */
void observe_conditional(std::size_t index, const Copies& copies) noexcept {
    if (copies.conditional == kNoHandle) {
        return;
    }
    const AuthoredNpc& npc = kAuthored[index];
    Observed& seen = g_observed[index];
    std::array<char, 160> line{};
    if (seen.conditional != copies.conditional) {
        seen.conditional = copies.conditional;
        (void)std::snprintf(line.data(), line.size(),
                            "handle=0x%08X flags=0x%X action=observed_only", copies.conditional,
                            copies.conditionalFlags);
        log_authored(npc, "conditional_copy", line.data());
    }
    if (copies.nativeCount == 0) {
        return;
    }
    const std::uint32_t authored = copies.native[0].handle;
    if (seen.duplicateConditional == copies.conditional && seen.duplicateAuthored == authored) {
        return;
    }
    seen.duplicateConditional = copies.conditional;
    seen.duplicateAuthored = authored;
    (void)std::snprintf(line.data(), line.size(),
                        "conditional_handle=0x%08X authored_handle=0x%08X action=observed_only",
                        copies.conditional, authored);
    log_authored(npc, "duplicate_observed", line.data());
}

/** Back to idle for a new cycle, no sooner than `delay` from now. */
void rearm(std::size_t index, std::uint64_t now, std::uint64_t delay) noexcept {
    Cycle& cycle = g_cycles[index];
    cycle.phase = Phase::idle;
    cycle.handle = kNoHandle;
    cycle.lastSkip = nullptr;
    cycle.lastAvailability = 0;
    cycle.lastPlace = 0;
    cycle.lastWaiting = 0;
    cycle.nextAttempt = now + delay;
}

/** One NPC's cycle for this update. */
void service_npc(std::size_t index, std::int32_t bubble, std::uint64_t now) noexcept {
    const AuthoredNpc& npc = kAuthored[index];
    Cycle& cycle = g_cycles[index];
    const Copies copies = read_copies(npc.entity);
    const std::span<const world_objects::NpcCopy> natives(copies.native.data(),
                                                          copies.nativeCount);
    const bool handleLive =
        std::any_of(natives.begin(), natives.end(), [&cycle](const world_objects::NpcCopy& copy) {
            return copy.handle == cycle.handle;
        });
    std::array<char, 128> detail{};
    // An accepted place is followed to its end even if the player walks out meanwhile.
    if (cycle.phase == Phase::awaitingNative) {
        detect_native(index, natives, now);
        return;
    }
    if (bubble != npc.bubble) {
        if (cycle.inBubble) {
            // Leaving pauses the cycle; a live authored actor is adopted again on return.
            cycle = {};
        }
        (void)std::snprintf(detail.data(), detail.size(), "bubble=%d current_bubble=%d",
                            npc.bubble, bubble);
        note_skip(index, kSkipWrongBubble, detail.data());
        return;
    }
    if (!cycle.inBubble) {
        // Entering the residence: the place goes out as soon as the Tower script's burst allows.
        cycle.inBubble = true;
        cycle.lastSkip = nullptr;
        cycle.nextAttempt = now;
    }
    if (!g_enabled[index].load(std::memory_order_acquire)) {
        note_skip(index, kSkipDisabled, "");
        return;
    }
    observe_conditional(index, copies);
    if (g_recover[index].exchange(false, std::memory_order_acq_rel)
        && cycle.phase == Phase::failed) {
        (void)std::snprintf(detail.data(), detail.size(),
                            "action=recover previous=cycle_failed handle=0x%08X", cycle.handle);
        log_authored(npc, "toggle", detail.data());
        rearm(index, now, 0);
    }
    if ((cycle.phase == Phase::waitingPerformance || cycle.phase == Phase::restored)
        && !handleLive) {
        // The actor is gone while the player stays: one new cycle, paced.
        (void)std::snprintf(detail.data(), detail.size(), "reason=native_gone handle=0x%08X",
                            cycle.handle);
        log_authored(npc, "rearm", detail.data());
        rearm(index, now, kRearmDelayMs);
    }
    switch (cycle.phase) {
    case Phase::waitingPerformance:
        poll_performance(index, now);
        return;
    case Phase::restored:
        (void)std::snprintf(detail.data(), detail.size(), "handle=0x%08X", cycle.handle);
        note_skip(index, kSkipAlreadyRestored, detail.data());
        return;
    case Phase::failed:
        (void)std::snprintf(detail.data(), detail.size(), "handle=0x%08X", cycle.handle);
        note_skip(index, kSkipCycleFailed, detail.data());
        return;
    case Phase::idle:
    case Phase::awaitingNative:
        break;
    }
    // An authored actor already live is adopted instead of placing another squad.
    const auto authored = std::find_if(natives.begin(), natives.end(), authored_shape);
    if (authored != natives.end()) {
        (void)std::snprintf(detail.data(), detail.size(), "handle=0x%08X flags=0x%X squad_sensor=1",
                            authored->handle, authored->flags);
        log_authored(npc, "adopt_native", detail.data());
        enter_waiting(index, authored->handle, "adopted", now);
        return;
    }
    if (!natives.empty()) {
        // A native copy that is not an authored actor still blocks a second squad.
        (void)std::snprintf(detail.data(), detail.size(), "handle=0x%08X flags=0x%X",
                            natives[0].handle, natives[0].flags);
        note_skip(index, kSkipAlreadyNative, detail.data());
        return;
    }
    if (now < cycle.nextAttempt) {
        return;
    }
    cycle.nextAttempt = now + kAvailabilityRetryMs;
    place_squad(index, copies, now);
}

// Area wake-up: the vanilla authored NPC the Tower script places first in its bubble, placed by
// us only when the script's own request left it missing. Independent of every toggle; nothing
// here is ever destroyed, and after the place the game owns the actor's lifecycle.

/** One vanilla authored NPC the wake-up guarantees in its bubble. */
struct WakeNpc {
    const char* area;
    const char* name;
    std::uint32_t entity;
    std::int32_t bubble;
    /** Confirmed squad row, or kAbsent to find it by its spawner, rule and actor. */
    std::uint32_t squadRow;
    std::uint32_t spawner;
    std::uint32_t spawnRule;
};
constexpr std::array kWake{
    WakeNpc{"Bazaar", "Hawthorne", 0x80C1CA4DU, 1, kAbsent, 0x80B4A28CU, 0x80B4A286U},
    // npc42: squad_runtime row 46318 status=ready, actor_tag 0x80B9ECB7 exact=1.
    WakeNpc{"Hangar", "Amanda", 0x80B9ECB7U, 7, 46318U, 0x80B4A982U, 0x80B4A96DU},
};

enum class WakePhase : std::uint8_t {
    idle,
    /** squads::place was accepted; waiting at most kDetectMs for its native actor. */
    awaitingNative,
    /** Present, placed or timed out: nothing more until the next visit. */
    done,
};

/** One wake-up visit. Only the game-thread service touches it. */
struct WakeCycle {
    WakePhase phase{WakePhase::idle};
    bool inBubble{};
    std::uint64_t nextAttempt{};
    std::uint64_t placedAt{};
    std::array<std::uint32_t, kCopyCapacity> baseline{};
    std::size_t baselineCount{};
    std::uint32_t lastAvailability{};
    std::uint32_t lastPlace{};
};
std::array<WakeCycle, kWake.size()> g_wake{};

void log_wake(const WakeNpc& npc, const char* stage, const char* detail) noexcept {
    core::log::writef(core::log::Channel::client, core::log::Level::warn,
                      "DEBUG_SAULO tower_area_wakeup area=%s name=%s stage=%s %s", npc.area,
                      npc.name, stage, detail);
}

/** Logs one wake-up line only when it differs from the last one `last` remembers. */
void note_wake(const WakeNpc& npc, std::uint32_t& last, const char* stage,
               const char* detail) noexcept {
    const std::uint32_t key = line_key(detail);
    if (last == key) {
        return;
    }
    last = key;
    log_wake(npc, stage, detail);
}

/** @return True when one of the squad's members is the NPC's actor class. */
[[nodiscard]] bool squad_has_actor(const sdk::Catalog& catalog, const format::Squad& squad,
                                   std::uint32_t entity) noexcept {
    const auto actors = catalog.actor_classes();
    for (const format::SquadMember& member : sdk::squad_members(catalog, squad)) {
        if (member.actorClassIndex < actors.size()
            && actors[member.actorClassIndex].definitionTag == entity) {
            return true;
        }
    }
    return false;
}

/**
 * The NPC's squad row in the bound scenario: the confirmed row, checked, or the one squad whose
 * spawner, spawn rule and member actor are the NPC's.
 * @return "none" when found.
 */
[[nodiscard]] const char* wake_row(const sdk::BoundView& view, const WakeNpc& npc,
                                   std::uint32_t& row) noexcept {
    row = kAbsent;
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    if (scenario == nullptr) {
        return "no_scenario";
    }
    const sdk::Catalog& catalog = *view.catalog;
    const auto table = catalog.squads();
    const auto is_npc = [&](const format::Squad& squad) noexcept {
        return squad.scenarioIndex == view.scenarioRow && squad.spawnerConfigTag == npc.spawner
               && squad.spawnRuleConfigTag == npc.spawnRule
               && squad_has_actor(catalog, squad, npc.entity);
    };
    if (npc.squadRow != kAbsent) {
        if (npc.squadRow >= table.size() || !is_npc(table[npc.squadRow])) {
            return "squad_row_mismatch";
        }
        row = npc.squadRow;
        return "none";
    }
    std::size_t matches = 0;
    for (const format::Squad& squad : sdk::scenario_squads(catalog, *scenario)) {
        if (is_npc(squad)) {
            row = static_cast<std::uint32_t>(&squad - table.data());
            ++matches;
        }
    }
    if (matches == 1) {
        return "none";
    }
    row = kAbsent;
    return matches == 0 ? "squad_not_found" : "squad_ambiguous";
}

/** Availability, then the one squads::place this visit makes, with the Squads page defaults. */
void place_wake(std::size_t index, std::span<const world_objects::NpcCopy> natives,
                std::uint64_t now) noexcept {
    const WakeNpc& npc = kWake[index];
    WakeCycle& cycle = g_wake[index];
    std::array<char, 192> line{};
    if (yield_to_script(now)) {
        (void)std::snprintf(line.data(), line.size(), "result=script_busy next_ms=%llu",
                            static_cast<unsigned long long>(kAvailabilityRetryMs));
        note_wake(npc, cycle.lastAvailability, "availability", line.data());
        return;
    }
    Bound bound{};
    const char* reason = bind_tower(bound);
    std::uint32_t row = kAbsent;
    if (none(reason)) {
        reason = wake_row(bound.view, npc, row);
    }
    if (!none(reason)) {
        (void)std::snprintf(line.data(), line.size(), "result=refused reason=%s next_ms=%llu",
                            reason, static_cast<unsigned long long>(kAvailabilityRetryMs));
        note_wake(npc, cycle.lastAvailability, "availability", line.data());
        return;
    }
    Counts counts{};
    const std::size_t count =
        default_counts(*bound.view.catalog, bound.view.catalog->squads()[row], counts);
    const std::span<const std::int32_t> requested = std::span(counts).first(count);
    const squads::Status available =
        squads::availability(bound.view, row, requested, squad_auth::Mode::mode0);
    (void)std::snprintf(line.data(), line.size(), "row=%u result=%s", row,
                        squads::status_name(available));
    note_wake(npc, cycle.lastAvailability, "availability", line.data());
    if (available != squads::Status::ready) {
        return;
    }
    cycle.baselineCount = natives.size();
    for (std::size_t copy = 0; copy < natives.size(); ++copy) {
        cycle.baseline[copy] = natives[copy].handle;
    }
    // The one placement this visit makes; a refusal placed nothing and waits for the next check.
    const squads::Status placed =
        squads::place(bound.view, row, requested, squad_auth::Mode::mode0);
    (void)std::snprintf(line.data(), line.size(), "row=%u result=%s", row,
                        placed == squads::Status::queued ? "queued"
                                                         : squads::status_name(placed));
    if (placed != squads::Status::queued) {
        note_wake(npc, cycle.lastPlace, "place", line.data());
        return;
    }
    log_wake(npc, "place", line.data());
    cycle.phase = WakePhase::awaitingNative;
    cycle.placedAt = now;
}

/** One wake-up NPC for this update: detect, then place only if still missing after the script. */
void service_wake(std::size_t index, std::int32_t bubble, std::uint64_t now) noexcept {
    const WakeNpc& npc = kWake[index];
    WakeCycle& cycle = g_wake[index];
    const Copies copies = read_copies(npc.entity);
    const std::span<const world_objects::NpcCopy> natives(copies.native.data(),
                                                          copies.nativeCount);
    std::array<char, 160> line{};
    if (cycle.phase == WakePhase::awaitingNative) {
        const auto baselineEnd =
            cycle.baseline.begin() + static_cast<std::ptrdiff_t>(cycle.baselineCount);
        for (const world_objects::NpcCopy& copy : natives) {
            if (std::find(cycle.baseline.begin(), baselineEnd, copy.handle) != baselineEnd) {
                continue;
            }
            (void)std::snprintf(line.data(), line.size(),
                                "result=native_detected handle=0x%08X flags=0x%X squad_sensor=%u "
                                "after_ms=%llu",
                                copy.handle, copy.flags, copy.squadSensor ? 1U : 0U,
                                static_cast<unsigned long long>(now - cycle.placedAt));
            log_wake(npc, "complete", line.data());
            cycle.phase = WakePhase::done;
            return;
        }
        if (now - cycle.placedAt >= kDetectMs) {
            (void)std::snprintf(line.data(), line.size(),
                                "result=timeout reason=native_not_detected after_ms=%llu",
                                static_cast<unsigned long long>(now - cycle.placedAt));
            log_wake(npc, "complete", line.data());
            cycle.phase = WakePhase::done;
        }
        return;
    }
    if (bubble != npc.bubble) {
        if (cycle.inBubble) {
            cycle = {};
        }
        return;
    }
    if (!cycle.inBubble) {
        cycle.inBubble = true;
        cycle.nextAttempt = now;
    }
    if (cycle.phase == WakePhase::done) {
        return;
    }
    if (!natives.empty()) {
        // Present, usually from the script's own place: the game controls it from here on.
        (void)std::snprintf(line.data(), line.size(),
                            "result=present handle=0x%08X flags=0x%X squad_sensor=%u",
                            natives[0].handle, natives[0].flags,
                            natives[0].squadSensor ? 1U : 0U);
        log_wake(npc, "detect", line.data());
        log_wake(npc, "skip", "reason=already_native");
        cycle.phase = WakePhase::done;
        return;
    }
    if (now < cycle.nextAttempt) {
        return;
    }
    cycle.nextAttempt = now + kAvailabilityRetryMs;
    place_wake(index, natives, now);
}

} // namespace

bool authored_entity(std::uint32_t entity) noexcept {
    return std::any_of(kAuthored.begin(), kAuthored.end(),
                       [entity](const AuthoredNpc& npc) { return npc.entity == entity; });
}

bool authored_enabled(Npc npc) noexcept {
    const auto index = static_cast<std::size_t>(npc);
    return index < kAuthoredCount && g_enabled[index].load(std::memory_order_acquire);
}

void set_authored_enabled(Npc npc, bool enabled) noexcept {
    const auto index = static_cast<std::size_t>(npc);
    if (index >= kAuthoredCount
        || g_enabled[index].exchange(enabled, std::memory_order_acq_rel) == enabled) {
        return;
    }
    if (enabled) {
        // Off to on: the game thread gives a failed cycle a fresh start in the bubble.
        g_recover[index].store(true, std::memory_order_release);
    }
    log_authored(kAuthored[index], "toggle",
                 enabled ? "enabled=1" : "enabled=0 live_actor=kept");
}

void service(std::int32_t towerBubble) noexcept {
    const std::uint64_t session =
        towerBubble >= 0 ? activity::membership::live_region_session(activity::kAbsentSessionId)
                         : activity::kAbsentSessionId;
    if (session != g_session) {
        // A new Tower session, or leaving it, forgets every cycle; the game tore its world down.
        g_cycles = {};
        g_observed = {};
        g_wake = {};
        g_session = session;
    }
    if (towerBubble < 0) {
        g_bubble = -1;
        return;
    }
    const std::uint64_t now = GetTickCount64();
    if (towerBubble != g_bubble) {
        g_bubble = towerBubble;
        g_bubbleSince = now;
    }
    // The wake-up NPCs first, so after the script they take the host ahead of our own squads.
    for (std::size_t index = 0; index < kWake.size(); ++index) {
        service_wake(index, towerBubble, now);
    }
    for (std::size_t index = 0; index < kAuthoredCount; ++index) {
        service_npc(index, towerBubble, now);
    }
}

void reset() noexcept {
    g_cycles = {};
    g_observed = {};
    g_wake = {};
    g_bubble = -1;
    g_session = activity::kAbsentSessionId;
}

} // namespace sunrise::client::hooks::entity_spawn_test::tower_npc_authored_restoration

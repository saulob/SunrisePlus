#include "activity_sdk_squad_runtime.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../middleware/content/packages/tables/region_reader.h"
#include "../../state/activity/runtime.h"
#include "../../state/build_data/runtime.h"
#include "../bap/runtime.h"
#include "host_runtime.h"

namespace sunrise::server::activity::activity_sdk_squads {
namespace {

namespace format = state::activity_sdk::format;
namespace layouts = state::build_data::scenarios;
namespace sdk = state::activity_sdk;

/** Spawn rules are type-66 slots; the squad Auth names one by its wire slot index. */
constexpr std::uint32_t kSpawnRuleSlotType = 66;
namespace squad_auth = middleware::bap::activity_message::squad_auth;
namespace tables = middleware::content::packages::tables;

/** Exact private route retained while a generated squad request is staged. */
struct PreparedSquad final {
    host::ScriptableTarget target{};
    layouts::RosterGroup generatedRosterGroup{};
    std::uint64_t activityClientGeneration{};
    std::int32_t effectiveRegion{-1};
    std::array<std::int8_t, 4> authoredProfile{};
};

/** Maps the SDK binding validator to this API's stable refusal surface. */
[[nodiscard]] Status binding_status(const sdk::BoundView& view,
                                    server::bap::ActivityLinkView& link) noexcept {
    if (view.catalog == nullptr || sdk::bound_activity(view) == nullptr
        || sdk::bound_scenario(view) == nullptr) {
        return Status::invalidView;
    }
    if (!state::activity::binding_matches(view.binding)) {
        return Status::staleBinding;
    }
    (void)server::bap::activity_link_view(view.binding, link);
    switch (
        sdk::revalidate(view, view.binding, link.matchingLinks, link.activityClientGeneration)) {
    case sdk::Status::ready:
        return link.effectiveRegion >= 0 ? Status::ready : Status::noActivityLink;
    case sdk::Status::missingClient:
    case sdk::Status::ambiguousClient:
        return Status::noActivityLink;
    case sdk::Status::staleSession:
        return Status::staleBinding;
    case sdk::Status::staleActivityClient:
        return Status::staleActivityClient;
    case sdk::Status::notReady:
    case sdk::Status::missing:
    case sdk::Status::wrongSdkBuild:
    case sdk::Status::catalogInvalid:
    case sdk::Status::wrongActivity:
    case sdk::Status::activityJoinNotExact:
    case sdk::Status::missingScenarioLink:
        return Status::invalidView;
    }
    return Status::invalidView;
}

/** Validates the exact authored member vector and every requested safe bound. */
[[nodiscard]] Status member_status(const sdk::Catalog& catalog,
                                   const format::Squad& squad,
                                   std::uint32_t squadRow,
                                   std::span<const std::int32_t> requestedCounts) noexcept {
    const auto members = sdk::squad_members(catalog, squad);
    if (members.size() < squad_auth::kMinimumRequestedCountLength
        || members.size() > squad_auth::kMaximumRequestedCountLength
        || requestedCounts.size() != members.size()) {
        return Status::memberCountMismatch;
    }
    for (std::size_t index = 0; index < members.size(); ++index) {
        const format::SquadMember& member = members[index];
        if (member.squadIndex != squadRow
            || member.memberOrdinal != static_cast<std::uint32_t>(index)) {
            return Status::invalidSquad;
        }
        if (requestedCounts[index] < 0) {
            return Status::memberCountOutOfRange;
        }
    }
    return Status::ready;
}

/** Copies the exact package actor profile shared by every positively requested member. */
[[nodiscard]] bool authored_profile(const sdk::Catalog& catalog,
                                    const format::Squad& squad,
                                    std::span<const std::int32_t> requestedCounts,
                                    std::array<std::int8_t, 4>& output,
                                    std::size_t& rejectedMember,
                                    const char*& rejection) noexcept {
    output = {};
    const auto members = sdk::squad_members(catalog, squad);
    rejectedMember = members.size();
    rejection = "no_profile_member";
    const auto actors = catalog.actor_classes();
    bool found = false;
    // A named type-2 member needs its parent's authored profile but zero loose actors.
    const bool loose = std::any_of(
        requestedCounts.begin(), requestedCounts.end(), [](auto count) { return count > 0; });
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (loose && requestedCounts[index] <= 0) {
            continue;
        }
        const format::SquadMember& member = members[index];
        if ((member.flags & format::kSquadMemberActorClassExact) == 0) {
            rejectedMember = index;
            rejection = "actor_class_not_exact";
            return false;
        }
        if (member.actorClassIndex >= actors.size()) {
            rejectedMember = index;
            rejection = "actor_class_index_invalid";
            return false;
        }
        const auto& candidate = actors[member.actorClassIndex].authoredSpawnProfile;
        if (found && candidate != output) {
            rejectedMember = index;
            rejection = "authored_profile_mismatch";
            return false;
        }
        output = candidate;
        found = true;
    }
    return found;
}
/** Checks the generated source slot before it can select a wire roster target. */
[[nodiscard]] bool valid_generated_slot(const sdk::Catalog& catalog,
                                        const format::Squad& squad,
                                        const format::Object*& object,
                                        const format::Slot*& slot) noexcept {
    object = nullptr;
    slot = nullptr;
    const auto objects = catalog.objects();
    const auto slots = catalog.slots();
    if (squad.objectIndex >= objects.size() || squad.slotIndex >= slots.size()) {
        return false;
    }
    object = &objects[squad.objectIndex];
    slot = &slots[squad.slotIndex];
    return slot->objectIndex == squad.objectIndex
           && slot->slotIndex <= (std::numeric_limits<std::uint16_t>::max)()
           && slot->slotType == format::kSquadSlotType
           && slot->componentClass == format::kSquadComponentClass
           && slot->senseSchema == format::kSquadSenseSchema
           && slot->authSchema == format::kSquadAuthSchema
           && (slot->flags & format::kSlotSchemaJoinExact) != 0;
}

/** @return True when two complete roster rows have the same key and wire slot layout. */
[[nodiscard]] bool same_roster_layout(const layouts::RosterGroup& left,
                                      const layouts::RosterGroup& right) noexcept {
    if (!layouts::valid_roster_group(left) || !layouts::valid_roster_group(right)
        || left.registryKey != right.registryKey || left.slotCount != right.slotCount) {
        return false;
    }
    for (std::size_t index = 0; index < left.slotCount; ++index) {
        if (left.slotTypes[index] != right.slotTypes[index]
            || left.slotFlags[index] != right.slotFlags[index]
            || left.slotIndices[index] != right.slotIndices[index]) {
            return false;
        }
    }
    return true;
}

/** Finds one exact type-1 slot in the roster's compressed wire order. */
[[nodiscard]] bool selected_slot_offset(const layouts::RosterGroup& group,
                                        const format::Slot& slot,
                                        std::uint16_t& output) noexcept {
    output = 0;
    if (!layouts::valid_roster_group(group)
        || slot.slotIndex > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    std::size_t matches = 0;
    for (std::size_t index = 0; index < group.slotCount; ++index) {
        if (group.slotIndices[index] != slot.slotIndex || group.slotTypes[index] != slot.slotType
            || (group.slotFlags[index] & layouts::kSlotAuthFlag) == 0) {
            continue;
        }
        output = static_cast<std::uint16_t>(index);
        ++matches;
    }
    return matches == 1;
}

/** Resolves one validated SDK row to a canonical group or an inline generated group. */
[[nodiscard]] Status resolve_target(const sdk::Catalog& catalog,
                                    const state::activity::SessionBinding& binding,
                                    const format::Scenario& scenario,
                                    const format::Squad& squad,
                                    const format::Occurrence& occurrence,
                                    const format::Slot& generatedSlot,
                                    const layouts::RosterGroup& generatedGroup,
                                    std::int32_t activityClientRegion,
                                    host::ScriptableTarget& output) noexcept {
    output = {};
    const auto bubbles = catalog.bubbles();
    const auto states = catalog.states();
    if (occurrence.bubbleIndex >= bubbles.size() || occurrence.stateIndex >= states.size()) {
        return Status::invalidSquad;
    }
    const format::Bubble& generatedBubble = bubbles[occurrence.bubbleIndex];
    const format::State& generatedState = states[occurrence.stateIndex];
    if (activityClientRegion < 0) {
        return Status::noActivityLink;
    }
    if (generatedBubble.scenarioIndex != squad.scenarioIndex
        || generatedState.scenarioIndex != squad.scenarioIndex
        || generatedState.bubbleIndex != occurrence.bubbleIndex
        || generatedBubble.bubbleOrdinal >= layouts::kBubbleCapacity
        || generatedState.stateOrdinal >= tables::kSliceSetIndexFactor) {
        return Status::invalidSquad;
    }
    const std::uint64_t authoredRegion =
        std::uint64_t{generatedBubble.bubbleOrdinal} * tables::kSliceSetIndexFactor
        + generatedState.stateOrdinal;
    if (authoredRegion > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
        return Status::invalidSquad;
    }
    const auto targetRegion = static_cast<std::int32_t>(authoredRegion);
    std::uint16_t slotOffset = 0;
    if (!selected_slot_offset(generatedGroup, generatedSlot, slotOffset)
        || binding.destination.packageNameLength == 0
        || binding.destination.packageNameLength > binding.destination.packageName.size()) {
        return Status::invalidSquad;
    }

    const std::string_view destination(
        reinterpret_cast<const char*>(binding.destination.packageName.data()),
        binding.destination.packageNameLength);
    layouts::Definition layout{};
    if (!state::build_data::find_scenario_layout(destination, layout) || layout.tag != scenario.tag
        || layout.rosterGroupCount > layout.rosterGroups.size()
        || layout.bubbleGroupCount > layout.bubbleGroups.size()) {
        return Status::targetUnavailable;
    }

    const std::uint32_t activeBubble =
        static_cast<std::uint32_t>(activityClientRegion) / tables::kSliceSetIndexFactor;
    const bool authoredBubbleActive = activeBubble == generatedBubble.bubbleOrdinal;
    std::size_t matches = 0;
    std::size_t activeMatches = 0;
    std::uint16_t selectedTable = 0;
    layouts::RosterGroup selectedGroup{};
    const auto consider = [&](std::uint16_t tableIndex, bool active) noexcept {
        layouts::RosterGroup candidate{};
        if (!state::build_data::find_roster_group(tableIndex, candidate)) {
            return false;
        }
        if (!same_roster_layout(generatedGroup, candidate)) {
            return true;
        }
        ++matches;
        if (active) {
            ++activeMatches;
            selectedTable = tableIndex;
            selectedGroup = candidate;
        }
        return true;
    };
    for (std::size_t index = 0; index < layout.rosterGroupCount; ++index) {
        if (!consider(layout.rosterGroups[index], authoredBubbleActive)) {
            return Status::targetUnavailable;
        }
    }
    for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
        const bool active =
            authoredBubbleActive && activeBubble < layouts::kBubbleCapacity
            && (layout.bubbleGroupMasks[index] & (std::uint64_t{1} << activeBubble)) != 0;
        if (!consider(layout.bubbleGroups[index], active)) {
            return Status::targetUnavailable;
        }
    }
    if (matches > 1) {
        return Status::ambiguousTarget;
    }
    if (matches == 1 && activeMatches == 1) {
        output.objectTag = selectedGroup.objectTag;
        output.registryKey = selectedGroup.registryKey;
        output.authSchema = generatedSlot.authSchema;
        output.rosterGroupIndex = selectedTable;
        output.rosterSlotOffset = slotOffset;
        output.slotIndex = static_cast<std::uint16_t>(generatedSlot.slotIndex);
        output.slotType = static_cast<std::uint8_t>(generatedSlot.slotType);
        return Status::ready;
    }

    output.objectTag = generatedGroup.objectTag;
    output.registryKey = generatedGroup.registryKey;
    output.authSchema = generatedSlot.authSchema;
    output.rosterGroupIndex = host::kGeneratedRosterGroupIndex;
    output.rosterSlotOffset = slotOffset;
    output.slotIndex = static_cast<std::uint16_t>(generatedSlot.slotIndex);
    output.sdkObjectIndex = squad.objectIndex;
    output.stateLocalRegion = targetRegion;
    output.slotType = static_cast<std::uint8_t>(generatedSlot.slotType);
    output.stateLocalRoster = true;
    return Status::ready;
}

/** DEBUG_SAULO: one result already logged on this thread during the diagnostic run. */
struct DebugSauloTraceStamp {
    std::uint32_t squadRow{};
    Status status{Status::ready};
    const char* reason{};
    std::size_t member{};
};

/** DEBUG_SAULO: log a confirmed Tower squad evaluation once per row, status and reason. */
void trace_tower_squad(const sdk::Catalog& catalog,
                       const format::Scenario& scenario,
                       const format::Squad& squad,
                       std::uint32_t squadRow,
                       std::span<const std::int32_t> requestedCounts,
                       Status status,
                       const char* reason,
                       std::size_t rejectedMember = static_cast<std::size_t>(-1),
                       const host::ScriptableTarget* target = nullptr) noexcept {
    const auto objects = catalog.objects();
    if (scenario.tag != 0x80B4A0F4U || squad.objectIndex >= objects.size()) {
        return;
    }
    const format::Object& object = objects[squad.objectIndex];
    const bool yuna = object.objectTag == 0x80B4A54AU && object.objectKey == 0x2EFB59ADU;
    const bool xur = object.objectTag == 0x80B4AD29U && object.objectKey == 0x728E75D1U;
    if (!yuna && !xur) {
        return;
    }
    // DEBUG_SAULO: availability can be queried each frame; retain only distinct diagnostics.
    static thread_local std::array<DebugSauloTraceStamp, 64> emitted{};
    static thread_local std::size_t emittedCount = 0;
    for (std::size_t index = 0; index < emittedCount; ++index) {
        const DebugSauloTraceStamp& prior = emitted[index];
        if (prior.squadRow == squadRow && prior.status == status && prior.reason == reason
            && prior.member == rejectedMember) {
            return;
        }
    }
    if (emittedCount == emitted.size()) {
        return;
    }
    emitted[emittedCount++] = {squadRow, status, reason, rejectedMember};
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "DEBUG_SAULO squad_runtime squad_row=%u object=0x%08X carrier=0x%08X "
                      "spawner=0x%08X spawnrule=0x%08X flags=0x%08X required=0x%08X "
                      "missing=0x%08X status=%s reason=%s",
                      squadRow,
                      object.objectTag,
                      object.objectKey,
                      squad.spawnerConfigTag,
                      squad.spawnRuleConfigTag,
                      squad.flags,
                      format::kSquadRunnableMask,
                      format::kSquadRunnableMask & ~squad.flags,
                      status_name(status),
                      reason);
    if (target != nullptr) {
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "DEBUG_SAULO squad_runtime_target squad_row=%u group=%u slot=%u "
                          "state_local=%u region=%d status=ready",
                          squadRow,
                          static_cast<unsigned>(target->rosterGroupIndex),
                          static_cast<unsigned>(target->slotIndex),
                          target->stateLocalRoster ? 1U : 0U,
                          target->stateLocalRegion);
    }
    if (status != Status::notRunnable || rejectedMember != static_cast<std::size_t>(-1)) {
        // DEBUG_SAULO: ready and profile refusals expose only real member and actor fields.
        const auto members = sdk::squad_members(catalog, squad);
        const auto actors = catalog.actor_classes();
        for (std::size_t index = 0; index < members.size() && index < requestedCounts.size();
             ++index) {
            const format::SquadMember& member = members[index];
            if (member.actorClassIndex < actors.size()) {
                const format::ActorClass& actor = actors[member.actorClassIndex];
                const auto& profile = actor.authoredSpawnProfile;
                core::log::writef(
                    core::log::Channel::server,
                    core::log::Level::warn,
                    "DEBUG_SAULO squad_runtime_member squad_row=%u member=%zu requested=%d "
                    "member_flags=0x%08X actor_class=%u exact=%u actor_tag=0x%08X "
                    "profile=%d,%d,%d,%d rejected=%u",
                    squadRow,
                    index,
                    requestedCounts[index],
                    member.flags,
                    member.actorClassIndex,
                    (member.flags & format::kSquadMemberActorClassExact) != 0 ? 1U : 0U,
                    actor.definitionTag,
                    static_cast<int>(profile[0]),
                    static_cast<int>(profile[1]),
                    static_cast<int>(profile[2]),
                    static_cast<int>(profile[3]),
                    index == rejectedMember ? 1U : 0U);
                if (yuna && actor.definitionTag == 0x80C93820U) {
                    core::log::writef(core::log::Channel::server,
                                      core::log::Level::warn,
                                      "DEBUG_SAULO yuna_path stage=actor_class tag=0x%08X "
                                      "squad_row=%u member=%zu",
                                      actor.definitionTag,
                                      squadRow,
                                      index);
                }
            } else {
                core::log::writef(core::log::Channel::server,
                                  core::log::Level::warn,
                                  "DEBUG_SAULO squad_runtime_member squad_row=%u member=%zu "
                                  "requested=%d member_flags=0x%08X actor_class=%u exact=%u "
                                  "rejected=%u",
                                  squadRow,
                                  index,
                                  requestedCounts[index],
                                  member.flags,
                                  member.actorClassIndex,
                                  (member.flags & format::kSquadMemberActorClassExact) != 0 ? 1U
                                                                                           : 0U,
                                  index == rejectedMember ? 1U : 0U);
            }
        }
    }
}

/** Resolves one public generated squad request without mutating transport state. */
[[nodiscard]] Status prepare(const sdk::BoundView& view,
                             std::uint32_t squadRow,
                             std::span<const std::int32_t> requestedCounts,
                             squad_auth::Mode mode,
                             PreparedSquad& output) noexcept {
    output = {};
    server::bap::ActivityLinkView link{};
    const Status liveStatus = binding_status(view, link);
    if (liveStatus != Status::ready) {
        if (view.catalog != nullptr && squadRow < view.catalog->squads().size()) {
            if (const format::Scenario* const bound = sdk::bound_scenario(view)) {
                trace_tower_squad(*view.catalog, *bound, view.catalog->squads()[squadRow],
                                  squadRow, requestedCounts, liveStatus, status_name(liveStatus));
            }
        }
        return liveStatus;
    }
    if (!squad_auth::valid_mode(mode)) {
        if (squadRow < view.catalog->squads().size()) {
            trace_tower_squad(*view.catalog, *sdk::bound_scenario(view),
                              view.catalog->squads()[squadRow], squadRow, requestedCounts,
                              Status::invalidMode, "invalid_mode");
        }
        return Status::invalidMode;
    }

    const sdk::Catalog& catalog = *view.catalog;
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    const auto squads = catalog.squads();
    if (scenario == nullptr || squadRow >= squads.size()) {
        return Status::invalidSquad;
    }
    const format::Squad& squad = squads[squadRow];
    if (squad.scenarioIndex != view.scenarioRow) {
        // DEBUG_SAULO: retain the exact preflight refusal for confirmed Tower squads.
        trace_tower_squad(catalog, *scenario, squad, squadRow, requestedCounts,
                          Status::wrongScenario, "wrong_scenario");
        return Status::wrongScenario;
    }
    if ((squad.flags & format::kSquadRunnableMask) != format::kSquadRunnableMask) {
        // DEBUG_SAULO: first and only flag-based not_runnable exit.
        trace_tower_squad(catalog, *scenario, squad, squadRow, requestedCounts,
                          Status::notRunnable, "flags_missing");
        return Status::notRunnable;
    }
    const Status members = member_status(catalog, squad, squadRow, requestedCounts);
    if (members != Status::ready) {
        // DEBUG_SAULO: member validation precedes authored profile evaluation.
        trace_tower_squad(catalog, *scenario, squad, squadRow, requestedCounts,
                          members, status_name(members));
        return members;
    }
    std::size_t rejectedMember = 0;
    const char* profileRejection = nullptr;
    if (!authored_profile(catalog, squad, requestedCounts, output.authoredProfile,
                          rejectedMember, profileRejection)) {
        // DEBUG_SAULO: second not_runnable exit, with the helper's exact failed check.
        trace_tower_squad(catalog, *scenario, squad, squadRow, requestedCounts,
                          Status::notRunnable, profileRejection, rejectedMember);
        return Status::notRunnable;
    }

    const auto occurrences = catalog.occurrences();
    if (squad.occurrenceIndex >= occurrences.size()) {
        // DEBUG_SAULO: no occurrence row survived for this squad.
        trace_tower_squad(catalog, *scenario, squad, squadRow, requestedCounts,
                          Status::invalidSquad, "occurrence_index_invalid");
        return Status::invalidSquad;
    }
    const format::Occurrence& occurrence = occurrences[squad.occurrenceIndex];
    if (occurrence.scenarioIndex != squad.scenarioIndex
        || occurrence.objectIndex != squad.objectIndex) {
        // DEBUG_SAULO: occurrence points at another scenario or object.
        trace_tower_squad(catalog, *scenario, squad, squadRow, requestedCounts,
                          Status::invalidSquad, "occurrence_join_mismatch");
        return Status::invalidSquad;
    }
    const format::Object* generatedObject = nullptr;
    const format::Slot* generatedSlot = nullptr;
    if (!valid_generated_slot(catalog, squad, generatedObject, generatedSlot)) {
        // DEBUG_SAULO: the generated source slot failed its exact schema checks.
        trace_tower_squad(catalog, *scenario, squad, squadRow, requestedCounts,
                          Status::invalidSquad, "generated_slot_invalid");
        return Status::invalidSquad;
    }
    if (!sdk::materialize_roster_group(catalog, *generatedObject, output.generatedRosterGroup)) {
        // DEBUG_SAULO: the generated object could not form a valid roster row.
        trace_tower_squad(catalog, *scenario, squad, squadRow, requestedCounts,
                          Status::invalidSquad, "generated_roster_invalid");
        output = {};
        return Status::invalidSquad;
    }

    const Status target = resolve_target(catalog,
                                         view.binding,
                                         *scenario,
                                         squad,
                                         occurrence,
                                         *generatedSlot,
                                         output.generatedRosterGroup,
                                         link.effectiveRegion,
                                         output.target);
    if (target != Status::ready) {
        // DEBUG_SAULO: target matching failed after profile and roster construction.
        trace_tower_squad(catalog, *scenario, squad, squadRow, requestedCounts,
                          target, "target_resolution");
        output = {};
        return target;
    }
    output.effectiveRegion = link.effectiveRegion;
    output.activityClientGeneration = link.activityClientGeneration;
    // DEBUG_SAULO: successful preparation is recorded for comparison with rejections.
    trace_tower_squad(catalog, *scenario, squad, squadRow, requestedCounts,
                      Status::ready, "ready", static_cast<std::size_t>(-1), &output.target);
    return Status::ready;
}

/** Opt-in needs one exact actor class for every positively requested member. */
state::gameplay::squad_entity_retirement::Eligibility
retirement_eligibility(const sdk::BoundView& view,
                       std::uint32_t squadRow,
                       std::span<const std::int32_t> counts,
                       const host::ScriptableTarget& target,
                       bool enabled) noexcept {
    state::gameplay::squad_entity_retirement::Eligibility result{};
    result.squad = {target.registryKey, target.slotIndex, target.slotType};
    if (!enabled || !view.catalog || target.slotType != 1) {
        return result;
    }
    const auto& catalog = *view.catalog;
    const auto squads = catalog.squads();
    if (squadRow >= squads.size()) {
        return result;
    }
    const auto members = sdk::squad_members(catalog, squads[squadRow]);
    const auto classes = catalog.actor_classes();
    if (members.size() != counts.size()) {
        return result;
    }
    std::uint32_t selected = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (counts[i] <= 0) {
            continue;
        }
        if ((members[i].flags & format::kSquadMemberActorClassExact) == 0
            || members[i].actorClassIndex >= classes.size()) {
            return result;
        }
        const auto rsat = classes[members[i].actorClassIndex].rsatTag;
        if (rsat == 0 || (selected != 0 && selected != rsat)) {
            return result;
        }
        selected = rsat;
    }
    if (selected == 0 || std::count_if(classes.begin(), classes.end(), [&](const auto& actor) {
                             return actor.rsatTag == selected;
                         }) != 1) {
        return result;
    }
    const auto occurrences = catalog.occurrences();
    const auto bubbles = catalog.bubbles();
    if (squads[squadRow].occurrenceIndex >= occurrences.size()) {
        return result;
    }
    const auto& occurrence = occurrences[squads[squadRow].occurrenceIndex];
    if (occurrence.bubbleIndex >= bubbles.size()
        || bubbles[occurrence.bubbleIndex].bubbleOrdinal >= 64) {
        return result;
    }
    result.rsatTag = selected;
    result.bubble = static_cast<std::uint8_t>(bubbles[occurrence.bubbleIndex].bubbleOrdinal);
    result.enabled = result.bubble < 64;
    return result;
}

} // namespace

/** Availability checks never register reuse before Auth delivery. */
/** The destination is another type-1 squad of the same object, sent as its wire slot. */
[[nodiscard]] Status resolve_destination(const sdk::Catalog& catalog,
                                         std::uint32_t squadRow,
                                         std::uint32_t registryKey,
                                         std::optional<std::uint32_t> destinationSquadRow,
                                         std::optional<squad_auth::Destination>& output) noexcept {
    output.reset();
    if (!destinationSquadRow.has_value()) {
        return Status::ready;
    }
    const auto squads = catalog.squads();
    if (squadRow >= squads.size() || *destinationSquadRow >= squads.size()
        || *destinationSquadRow == squadRow
        || squads[*destinationSquadRow].objectIndex != squads[squadRow].objectIndex) {
        return Status::invalidSquad;
    }
    const format::Object* object = nullptr;
    const format::Slot* slot = nullptr;
    if (!valid_generated_slot(catalog, squads[*destinationSquadRow], object, slot)) {
        return Status::invalidSquad;
    }
    output = squad_auth::Destination{registryKey, static_cast<std::uint16_t>(slot->slotIndex)};
    return Status::ready;
}

/** The rule is a type-66 slot of the squad's own object, sent as its wire slot. */
[[nodiscard]] Status resolve_spawn_rule(const sdk::Catalog& catalog,
                                        std::uint32_t squadRow,
                                        std::uint32_t registryKey,
                                        std::optional<std::uint32_t> spawnRuleSlotRow,
                                        std::optional<squad_auth::SpawnRule>& output) noexcept {
    output.reset();
    if (!spawnRuleSlotRow.has_value()) {
        return Status::ready;
    }
    const auto squads = catalog.squads();
    const auto slots = catalog.slots();
    if (squadRow >= squads.size() || *spawnRuleSlotRow >= slots.size()) {
        return Status::invalidSquad;
    }
    const format::Slot& slot = slots[*spawnRuleSlotRow];
    if (slot.objectIndex != squads[squadRow].objectIndex || slot.slotType != kSpawnRuleSlotType
        || slot.slotIndex > (std::numeric_limits<std::uint16_t>::max)()) {
        return Status::invalidSquad;
    }
    output = squad_auth::SpawnRule{registryKey, static_cast<std::uint16_t>(slot.slotIndex)};
    return Status::ready;
}

/**
 * Checks one placement without queuing it; name and retirement choices are not yet validated.
 * @param destinationSquadRow Optional actor-spawn destination squad of the same object.
 * @param spawnRuleSlotRow Optional type-66 rule slot of the same object.
 * @return `ready`, or the refusal a placement would get.
 */
Status availability(const sdk::BoundView& view,
                    std::uint32_t squadRow,
                    std::span<const std::int32_t> requestedCounts,
                    squad_auth::Mode mode,
                    std::optional<std::uint32_t> nameHash,
                    bool retireOnReturn,
                    std::optional<std::uint32_t> destinationSquadRow,
                    std::optional<std::uint32_t> spawnRuleSlotRow) noexcept {
    (void)nameHash;
    (void)retireOnReturn;
    PreparedSquad prepared{};
    const Status status = prepare(view, squadRow, requestedCounts, mode, prepared);
    if (status != Status::ready) {
        return status;
    }
    std::optional<squad_auth::Destination> destination{};
    const Status destinationStatus = resolve_destination(
        *view.catalog, squadRow, prepared.target.registryKey, destinationSquadRow, destination);
    if (destinationStatus != Status::ready) {
        return destinationStatus;
    }
    std::optional<squad_auth::SpawnRule> spawnRule{};
    return resolve_spawn_rule(
        *view.catalog, squadRow, prepared.target.registryKey, spawnRuleSlotRow, spawnRule);
}

/** Queues one preflighted generated squad through the proved private type-1 route. */
Status place(const sdk::BoundView& view,
             std::uint32_t squadRow,
             std::span<const std::int32_t> requestedCounts,
             squad_auth::Mode mode,
             std::optional<std::uint32_t> nameHash,
             bool retireOnReturn,
             std::optional<std::uint32_t> destinationSquadRow,
             std::optional<std::uint32_t> spawnRuleSlotRow) noexcept {
    PreparedSquad prepared{};
    const Status status = prepare(view, squadRow, requestedCounts, mode, prepared);
    if (status != Status::ready) {
        return status;
    }
    std::optional<squad_auth::Destination> destination{};
    const Status destinationStatus = resolve_destination(
        *view.catalog, squadRow, prepared.target.registryKey, destinationSquadRow, destination);
    if (destinationStatus != Status::ready) {
        return destinationStatus;
    }
    std::optional<squad_auth::SpawnRule> spawnRule{};
    const Status ruleStatus = resolve_spawn_rule(
        *view.catalog, squadRow, prepared.target.registryKey, spawnRuleSlotRow, spawnRule);
    if (ruleStatus != Status::ready) {
        return ruleStatus;
    }
    if (server::bap::request_activity_squad_override(
            view.binding,
            prepared.target,
            prepared.target.stateLocalRoster ? &prepared.generatedRosterGroup : nullptr,
            requestedCounts,
            mode,
            nameHash,
            prepared.effectiveRegion,
            prepared.activityClientGeneration,
            nullptr,
            prepared.authoredProfile,
            retirement_eligibility(
                view, squadRow, requestedCounts, prepared.target, retireOnReturn),
            destination,
            spawnRule)) {
        return Status::queued;
    }
    return Status::refused;
}

/** Queues one preflighted squad only through an exact unarmed Host revision. */
Status place_reserved(const sdk::BoundView& view,
                      std::uint32_t squadRow,
                      std::span<const std::int32_t> requestedCounts,
                      squad_auth::Mode mode,
                      const host::ScriptableOutputReservation& reservation,
                      std::optional<std::uint32_t> nameHash,
                      bool retireOnReturn,
                      std::optional<std::uint32_t> destinationSquadRow,
                      std::optional<std::uint32_t> spawnRuleSlotRow) noexcept {
    PreparedSquad prepared{};
    const Status status = prepare(view, squadRow, requestedCounts, mode, prepared);
    if (status != Status::ready) {
        return status;
    }
    std::optional<squad_auth::Destination> destination{};
    const Status destinationStatus = resolve_destination(
        *view.catalog, squadRow, prepared.target.registryKey, destinationSquadRow, destination);
    if (destinationStatus != Status::ready) {
        return destinationStatus;
    }
    std::optional<squad_auth::SpawnRule> spawnRule{};
    const Status ruleStatus = resolve_spawn_rule(
        *view.catalog, squadRow, prepared.target.registryKey, spawnRuleSlotRow, spawnRule);
    if (ruleStatus != Status::ready) {
        return ruleStatus;
    }
    if (server::bap::request_activity_squad_override(
            view.binding,
            prepared.target,
            prepared.target.stateLocalRoster ? &prepared.generatedRosterGroup : nullptr,
            requestedCounts,
            mode,
            nameHash,
            prepared.effectiveRegion,
            prepared.activityClientGeneration,
            &reservation,
            prepared.authoredProfile,
            retirement_eligibility(
                view, squadRow, requestedCounts, prepared.target, retireOnReturn),
            destination,
            spawnRule)) {
        return Status::queued;
    }
    return Status::refused;
}

/** Returns the concise operator and VM diagnostic name for one result. */
const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::ready:
        return "ready";
    case Status::queued:
        return "queued";
    case Status::invalidView:
        return "invalid_view";
    case Status::staleBinding:
        return "stale_binding";
    case Status::staleActivityClient:
        return "stale_activity_client";
    case Status::invalidSquad:
        return "invalid_squad";
    case Status::wrongScenario:
        return "wrong_scenario";
    case Status::notRunnable:
        return "not_runnable";
    case Status::invalidMode:
        return "invalid_mode";
    case Status::memberCountMismatch:
        return "member_count_mismatch";
    case Status::memberCountOutOfRange:
        return "member_count_out_of_range";
    case Status::noActivityLink:
        return "no_activity_link";
    case Status::targetUnavailable:
        return "target_unavailable";
    case Status::ambiguousTarget:
        return "ambiguous_target";
    case Status::outputBusy:
        return "output_busy";
    case Status::refused:
        return "refused";
    }
    return "unknown";
}

} // namespace sunrise::server::activity::activity_sdk_squads

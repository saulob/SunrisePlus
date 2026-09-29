#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::client::hooks::world_objects {

/** The placed-entry identity value that means the entry carries none. */
constexpr std::uint64_t kAbsentPlacementIdentity = 0xFFFFFFFFFFFFFFFFULL;

/**
 * One live placed object joined to its package object-list identity.
 * `placementIdentity` is the placed-entry `+0x70` value the datum retains at `+0x90`. It is zero
 * when the datum could not be read, and `kAbsentPlacementIdentity` when the entry has none.
 */
struct Instance final {
    std::uint64_t placementIdentity{};
    std::uint32_t objectListTag{};
    std::uint32_t entryIndex{};
    std::uint32_t handle{0xFFFFFFFFU};
    std::uint32_t generation{0xFFFFFFFFU};
};

/** Bounded registry health for the technical-details surface. */
struct Diagnostics final {
    std::size_t liveCount{};
    std::uint64_t overflowCount{};
    bool installed{};
};

/** Installs the placed-object lifetime capture. */
[[nodiscard]] bool install() noexcept;

/** Removes the capture after every replacement and trampoline call is idle. */
[[nodiscard]] bool uninstall() noexcept;

/** @return True while both lifetime hooks are attached and accepting observations. */
[[nodiscard]] bool is_installed() noexcept;

/**
 * Finds generation-valid live instances for one exact package placement.
 * @param objectListTag Package object-list tag retained by the live datum.
 * @param entryIndex Exact entry in that object list.
 * @param output Receives as many simultaneous live instances as fit.
 * @return Total matching live instances, which can be greater than output.size().
 */
[[nodiscard]] std::size_t
find(std::uint32_t objectListTag, std::uint32_t entryIndex, std::span<Instance> output) noexcept;

/** @return Current bounded-registry counters. */
[[nodiscard]] Diagnostics diagnostics() noexcept;

/**
 * @return The object datum whose self handle still equals `handle`, or null when the layout is
 * unbound or the slot now belongs to another object.
 */
[[nodiscard]] std::byte* live_datum(std::uint32_t handle) noexcept;

/**
 * Destroys one live object through the native logical destroy, the teardown that also releases
 * its simulation entity. Call it from the game thread.
 * @return True when the handle was live and the destroy ran.
 */
[[nodiscard]] bool logical_destroy_object(std::uint32_t handle) noexcept;

/**
 * One live copy of a watched NPC. `origin` names how it was created: sunrise_factory,
 * native_instantiate, native_create_entity, native_factory or other. Only a sunrise_factory
 * handle the caller itself created is the caller's to destroy.
 */
struct NpcCopy final {
    std::uint32_t handle{};
    const char* origin{};
    /** Creation descriptor flags (+0x68): 0x380 on the squad path, 0x300 from the factory. */
    std::uint32_t flags{};
    /** Self-relative auxiliary payload offset (+0x78); zero when the descriptor carries none. */
    std::int64_t auxRelative{};
    /** The auxiliary payload lists the squad-sensor class. */
    bool squadSensor{};
};

/**
 * Lists the watched NPC's copies whose datum is still live, as the allocate tracer saw them.
 * @return How many were written to `output`.
 */
[[nodiscard]] std::size_t live_npc_copies(std::uint32_t entityTag,
                                          std::span<NpcCopy> output) noexcept;

/** DEBUG_SAULO npc_idle_trace: publishes one watched NPC's resolved definition pointer. */
void note_npc_definition(std::uint32_t entityTag, std::uintptr_t definition) noexcept;

/** @return How many NPCs the idle tracer watches. */
[[nodiscard]] std::size_t npc_trace_count() noexcept;

/** @return The entity tag of one watched NPC, or 0 past the end. */
[[nodiscard]] std::uint32_t npc_trace_entity(std::size_t index) noexcept;

/** Runs one throttled, bounded, read-only idle-trace probe step. Call from one game thread. */
void trace_npc_actors() noexcept;

} // namespace sunrise::client::hooks::world_objects

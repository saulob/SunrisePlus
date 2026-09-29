#pragma once
#include <cstdint>

namespace sunrise::client::hooks::entity_spawn_test::tower_npc_authored_restoration {

/**
 * Tower NPCs restored as their own authored squad: squads::place brings back the native actor,
 * then its confirmed idle state is played through its performance sensor. No factory copy,
 * actor carrier or roster change is involved.
 */
enum class Npc : std::uint8_t {
    yuna,
    saladin,
    xur,
};

/** @return True for the entity of an authored-squad NPC, which no factory path may create. */
[[nodiscard]] bool authored_entity(std::uint32_t entity) noexcept;

/** @return Whether entering the NPC's bubble may place its squad. Every NPC starts enabled. */
[[nodiscard]] bool authored_enabled(Npc npc) noexcept;

/**
 * Allows or stops future placements. Turning it off never destroys anything: a live NPC stays
 * until the game unloads it.
 */
void set_authored_enabled(Npc npc, bool enabled) noexcept;

/**
 * Game thread. First the area wake-up, independent of every toggle: Hawthorne in the Bazaar and
 * Amanda in the Hangar are placed once per visit when still missing after the Tower script's own
 * request. Then, in each NPC's own bubble, adopt a live authored actor or place its squad once,
 * and play its confirmed idle state as soon as the world is ready for it. No request goes out
 * while the Tower script's first burst of a bubble holds the host.
 * @param towerBubble The current Tower bubble, or -1 outside the Tower.
 */
void service(std::int32_t towerBubble) noexcept;

/** Forgets every restoration cycle; nothing is destroyed. */
void reset() noexcept;

} // namespace sunrise::client::hooks::entity_spawn_test::tower_npc_authored_restoration

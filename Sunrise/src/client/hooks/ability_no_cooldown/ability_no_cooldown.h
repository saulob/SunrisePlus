#pragma once

namespace sunrise::client::hooks::ability_no_cooldown {

/**
 * Resolves the current-ability getter, the handle tables and the native ability-energy
 * adjustment, and attaches the getter observer that records Grenade, Super, Melee and Class
 * Ability owners.
 * @return True when every required energy target resolved and the getter detour attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the getter observer. */
void uninstall() noexcept;

/**
 * While an energy option is on and the player is in a world, adds one full unit to that ability
 * through the game's own adjustment; Super gets that unit once after each use ends, because a
 * roaming Super spends its energy as its duration. Drops every recorded owner when the player
 * leaves the world.
 * Call per frame on the game thread.
 */
void poll() noexcept;

} // namespace sunrise::client::hooks::ability_no_cooldown

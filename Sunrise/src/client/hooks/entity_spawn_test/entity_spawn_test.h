#pragma once

namespace sunrise::client::hooks::entity_spawn_test {

/**
 * Resolves the native spawner functions and attaches the PlayerComponentUpdate detour that
 * services the Tower NPC restoration: the factory NPCs and scenery, and the authored squads.
 * @return True when every signature was unique and the detour attached.
 */
[[nodiscard]] bool install() noexcept;

/**
 * Detaches the update detour and forgets every handle. Nothing is destroyed; the world owns what
 * it holds.
 */
void uninstall() noexcept;

} // namespace sunrise::client::hooks::entity_spawn_test

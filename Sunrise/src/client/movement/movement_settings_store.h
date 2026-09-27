#pragma once

#include <cstdint>

namespace sunrise::client::movement {

/** Default distance, in world units along the camera's forward vector. */
inline constexpr float kDefaultDistance = 10.0F;
/** Smallest offered distance. Zero would leave the key bound to nothing visible. */
inline constexpr float kMinimumDistance = 1.0F;
/** Largest offered distance. Past this a press reliably lands through a wall or the floor. */
inline constexpr float kMaximumDistance = 100.0F;
/** No key is bound until one is picked, so a fresh install cannot fire a movement feature. */
inline constexpr std::uint32_t kNoKey = 0;

/** Default fly speed, in world units per second. */
inline constexpr float kDefaultFlySpeed = 15.0F;
/** Slowest offered fly speed. Below this a press does not visibly move the player. */
inline constexpr float kMinimumFlySpeed = 1.0F;
/** Fastest offered fly speed. Past this the player outruns what the map streams in. */
inline constexpr float kMaximumFlySpeed = 100.0F;

/** Default movement speed, in world units per second. Matches the fly default. */
inline constexpr float kDefaultMovementSpeed = 15.0F;
/** Slowest offered movement speed. */
inline constexpr float kMinimumMovementSpeed = 1.0F;
/** Fastest offered movement speed. The same ceiling as fly, for the same streaming reason. */
inline constexpr float kMaximumMovementSpeed = 100.0F;
/** Default jump height multiplier for new settings. */
inline constexpr float kDefaultJumpHeightMultiplier = 2.0F;
/** Multiplier used in place of a non-finite value. It leaves the native jump unchanged. */
inline constexpr float kSafeJumpHeightMultiplier = 1.0F;
/** Smallest jump height multiplier offered by the interface. */
inline constexpr float kMinimumJumpHeightMultiplier = 1.0F;
/** Largest jump height multiplier offered by the interface. */
inline constexpr float kMaximumJumpHeightMultiplier = 10.0F;

/** Runtime movement configuration. This module owns it; Core settings do not carry it. */
struct Settings {
    bool enabled{false};
    float distance{kDefaultDistance};
    std::uint32_t virtualKey{kNoKey};
    bool noclipEnabled{false};
    std::uint32_t noclipToggleKey{kNoKey};
    /** The jump key comes from the account binding, so none is stored here. */
    bool swordSkateEnabled{false};
    bool flyEnabled{false};
    std::uint32_t flyToggleKey{kNoKey};
    /** World units per second while a direction is pressed. */
    float flySpeed{kDefaultFlySpeed};
    /** Ground movement at a set horizontal speed. Fly takes precedence; noclip carries it. */
    bool movementSpeedEnabled{false};
    std::uint32_t movementSpeedToggleKey{kNoKey};
    /** World units per second along the ground while a direction is pressed. */
    float movementSpeed{kDefaultMovementSpeed};
    bool jumpHeightEnabled{false};
    float jumpHeightMultiplier{kDefaultJumpHeightMultiplier};
};

/**
 * Resolves the configuration file and loads it when one exists.
 * @param module Loaded DLL used to resolve the owned artifact directory.
 */
void initialize(void* module) noexcept;

/** Drops the runtime configuration and the resolved file path. */
void shutdown() noexcept;

/** @return One lock-consistent copy of the current configuration. */
[[nodiscard]] Settings get() noexcept;

/**
 * Publishes one configuration and writes it straight to disk.
 * @param settings Candidate configuration, refused when a field is out of range.
 * @return True when the value was published. A failed write is logged, not returned.
 */
bool publish(const Settings& settings) noexcept;

} // namespace sunrise::client::movement

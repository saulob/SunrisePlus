#include "entity_spawn_test.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/authored_placement_reader.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/activity/runtime.h"
#include "../../content/items/packages/internal.h"
#include "../../executable/image.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/registry.h"
#include "../../patterns/signature_text.h"
#include "../teleport/runtime.h"
#include "../world_objects/world_object_registry.h"
#include "tower_npc_authored_restoration.h"

namespace sunrise::client::hooks::entity_spawn_test {
namespace {

using patterns::signature;
using patterns::signature_length;

constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFFU;

// Sunrise-Sandbox spawner signatures (6aae441, before 5398463 replaced them with RVAs). The
// 86657 image's .pdata has a function start at each Sandbox RVA whose unwind prologue matches
// these bytes, so they locate the same functions here without relying on those RVAs.

/** Builds a placement from a tag's placement source; returns nonzero on success. */
constexpr std::string_view kPlacementInitializeText =
    "89 54 24 10 53 48 83 EC 20 48 8B D9 83 FA FF 0F 84 ? ? ? ? 48 8D 54 24 30 "
    "48 8D 4C 24 38 E8 ? ? ? ? 8B 44 24 30 83 F8 FF 0F 84 ? ? ? ? 48 8B 15 ? ? ? ?";
constexpr auto kPlacementInitialize =
    signature<signature_length(kPlacementInitializeText)>(kPlacementInitializeText);

/** Builds a placement straight from the resolved definition; its first call is the resolver. */
constexpr std::string_view kDirectInitializeText =
    "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 83 FA FF 74 33 8B CA E8 ? ? ? ? "
    "48 C7 47 30 00 00 00 00 48 8B CF 48 C7 47 10 00 00 00 00";
constexpr auto kDirectInitialize =
    signature<signature_length(kDirectInitializeText)>(kDirectInitializeText);

/** `factory(out, descriptor)` forwards to the datum allocator as `(out, descriptor, -1, -1)`. */
constexpr std::string_view kObjectFactoryText =
    "40 53 48 83 EC 20 41 83 C9 FF 41 83 C8 FF 48 8B D9 E8 ? ? ? ? 48 8B C3 "
    "48 83 C4 20 5B C3";
constexpr auto kObjectFactory = signature<signature_length(kObjectFactoryText)>(kObjectFactoryText);

/** Writes rotation then position into a live object datum at +0xA0. */
constexpr std::string_view kObjectTransformText =
    "48 89 5C 24 10 57 48 83 EC 70 0F 29 74 24 60 48 8B 05 ? ? ? ? 48 33 C4 "
    "48 89 44 24 50 0F 10 02 48 8B F9 0F 11 81 A0 00 00 00 0F 10 72 10 "
    "0F 29 74 24 30 E8 ? ? ? ? 8B D8 E8 ? ? ? ?";
constexpr auto kObjectTransform =
    signature<signature_length(kObjectTransformText)>(kObjectTransformText);

/**
 * Sandbox PlayerComponentUpdate (a6925fe moved the spawner here from SummonUpdate). Three
 * pointer arguments; the Sandbox services its queue after the original returns.
 */
constexpr std::string_view kPlayerComponentUpdateText =
    "48 89 5C 24 10 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 70 45 33 E4 "
    "48 89 B4 24 A0 00 00 00 41 8B FC 48 8D 99 FC 02 00 00 4D 8B F0 4C 8B FA";
constexpr auto kPlayerComponentUpdate =
    signature<signature_length(kPlayerComponentUpdateText)>(kPlayerComponentUpdateText);

constexpr std::array kNativePatterns{
    patterns::Pattern{"spawn_placement_initialize", kPlacementInitialize},
    patterns::Pattern{"spawn_direct_initialize", kDirectInitialize},
    patterns::Pattern{"spawn_object_factory", kObjectFactory},
    patterns::Pattern{"spawn_object_transform", kObjectTransform},
    patterns::Pattern{"spawn_player_component_update", kPlayerComponentUpdate},
};
constexpr std::size_t kInitializeSlot = 0;
constexpr std::size_t kDirectSlot = 1;
constexpr std::size_t kFactorySlot = 2;
constexpr std::size_t kTransformSlot = 3;
constexpr std::size_t kUpdateSlot = 4;

/** The resolver call inside the direct initializer: `E8 rel32` at +0x16. */
constexpr std::size_t kResolverCallOperand = 0x17;
constexpr std::size_t kResolverCallEnd = 0x1B;

/** Sandbox controlled test: the updated object's handle at +0x2C, compared by datum index. */
constexpr std::size_t kUpdatedObjectHandle = 0x2C;
constexpr std::uint32_t kHandleIndexMask = 0x1FFFU;

/** Object type byte in a resolved definition; these types need a transform after creation. */
constexpr std::size_t kDefinitionObjectType = 0x96;
[[nodiscard]] constexpr bool needs_activation(std::uint8_t type) noexcept {
    return type == 8 || type == 11 || type == 20 || type == 21;
}

// Placement storage: header, then an in-place payload the initializer builds the descriptor in.
// +0x00 descriptor offset from storage base (written by the initializer)
// +0x10 zero, +0x18 invalid datum, +0x20 payload capacity, +0x28 payload alignment, +0x30 zero
constexpr std::size_t kPlacementHeaderBytes = 0x40;
constexpr std::size_t kPlacementPayloadBytes = 0x800;
constexpr std::size_t kDescriptorRotation = 0x10;
constexpr std::size_t kDescriptorPosition = 0x20;

constexpr float kSpawnScale = 1.0F;
constexpr std::string_view kTowerActivity = "city_tower_social_d2";
constexpr std::int32_t kBazaarBubble = 1;
constexpr std::uint64_t kAutoRetryMs = 3000;
constexpr std::uint8_t kActivationAttempts = 4;

using PlacementInitialize = std::uint8_t(__fastcall*)(void*, std::uint32_t);
using ObjectFactory = std::uint32_t*(__fastcall*)(std::uint32_t*, void*);
using ObjectTransform = void(__fastcall*)(void*, const float*);
using TagResolver = const std::byte*(__fastcall*)(std::uint32_t);
using PlayerComponentUpdate = void(__fastcall*)(void*, void*, void*);

struct Natives {
    PlacementInitialize initialize{};
    PlacementInitialize direct{};
    ObjectFactory factory{};
    ObjectTransform transform{};
    TagResolver resolver{};
};

struct alignas(16) PlacementStorage {
    std::array<std::byte, kPlacementHeaderBytes + kPlacementPayloadBytes> bytes{};
};

/** ObjectTransform input: quaternion, then position and scale. */
using Transform = std::array<float, 8>;

/** One created object that still needs its transform applied on a later frame. */
struct Activation {
    std::uint32_t handle{kInvalidHandle};
    Transform transform{};
    std::uint8_t attempts{};
};

/** Exception facts copied out of the factory's SEH filter. */
struct Fault {
    DWORD code{};
    void* address{};
};

hooking::detour::Handle g_updateHook{};
/** Written by install before the release store, read after the acquire load. */
Natives g_natives{};
std::atomic_bool g_installed{};

/** Serializes the update-side service, as the Sandbox's request and activation locks do. */
std::atomic_flag g_servicing = ATOMIC_FLAG_INIT;
std::atomic_bool g_activationQueued{};
/** Only the service holding g_servicing touches this. */
Activation g_activation{};
/** Set once a factory call ran in this controlled update; the scenery pass then waits. */
bool g_factoryUsed{};

/** Writes one formatted line, cut at the buffer when snprintf reports a longer one. */
void log_line(std::span<const char> buffer, int written) noexcept {
    if (written <= 0 || buffer.empty()) {
        return;
    }
    const std::size_t length = static_cast<std::size_t>(written) < buffer.size()
                                   ? static_cast<std::size_t>(written)
                                   : buffer.size() - 1;
    core::log::write(core::log::Channel::client, core::log::Level::warn, {buffer.data(), length});
}

void log_failure_as(const char* label, const char* stage, std::uint32_t tag,
                    const char* reason) noexcept {
    std::array<char, 224> line{};
    log_line(line, std::snprintf(line.data(), line.size(),
                                 "DEBUG_SAULO %s stage=%s tag=0x%08X "
                                 "result=fail reason=%s",
                                 label, stage, tag, reason));
}

/** @return The address relative to the game image, for comparing with static analysis. */
template <typename Pointer> [[nodiscard]] unsigned long long rva(Pointer address) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    return value != 0 ? static_cast<unsigned long long>(value - base) : 0ULL;
}

/** @return True when the address lies inside the mapped game image. */
[[nodiscard]] bool in_game_image(const void* address) noexcept {
    const auto* const base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
    if (base == nullptr || address == nullptr) {
        return false;
    }
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* const target = static_cast<const std::byte*>(address);
    return target >= base && target < base + nt->OptionalHeader.SizeOfImage;
}

/** Finds the five Sandbox functions in one sweep and decodes the resolver from the direct init. */
[[nodiscard]] bool resolve_natives(std::byte*& update) noexcept {
    update = nullptr;
    executable::ExecutableImage main{};
    if (!executable::inspect_main_module(main)) {
        log_failure_as("tower_npc_restore", "natives", 0, "main_image_unavailable");
        return false;
    }
    std::array<patterns::ImageRange, executable::kPeSectionLimit> ranges{};
    for (std::size_t index = 0; index < main.count; ++index) {
        ranges[index] = patterns::ImageRange{main.sections[index]};
    }
    std::array<patterns::Match, kNativePatterns.size()> matches{};
    (void)patterns::resolve_all(std::span(ranges.data(), main.count), kNativePatterns, matches);
    bool complete = true;
    for (std::size_t index = 0; index < matches.size(); ++index) {
        if (matches[index].status != patterns::MatchStatus::unique) {
            std::array<char, 160> line{};
            log_line(line,
                     std::snprintf(line.data(), line.size(),
                                   "DEBUG_SAULO tower_npc_restore stage=natives result=fail "
                                   "signature=%.*s status=%u",
                                   static_cast<int>(kNativePatterns[index].name.size()),
                                   kNativePatterns[index].name.data(),
                                   static_cast<unsigned>(matches[index].status)));
            complete = false;
        }
    }
    if (!complete) {
        return false;
    }
    std::byte* const direct = matches[kDirectSlot].address;
    g_natives.initialize =
        reinterpret_cast<PlacementInitialize>(matches[kInitializeSlot].address);
    g_natives.direct = reinterpret_cast<PlacementInitialize>(direct);
    g_natives.factory = reinterpret_cast<ObjectFactory>(matches[kFactorySlot].address);
    g_natives.transform = reinterpret_cast<ObjectTransform>(matches[kTransformSlot].address);
    g_natives.resolver = reinterpret_cast<TagResolver>(
        patterns::resolve_relative(direct + kResolverCallOperand, direct + kResolverCallEnd));
    update = matches[kUpdateSlot].address;

    // Static analysis of the 86657 image predicts 4B25F0 / 4B2570 / 56D990 / 559B10 / 1258970
    // and BB0DB0 for the update.
    std::array<char, 288> line{};
    log_line(line,
             std::snprintf(line.data(), line.size(),
                           "DEBUG_SAULO tower_npc_restore stage=natives result=ok "
                           "initialize=+0x%llX direct=+0x%llX factory=+0x%llX transform=+0x%llX "
                           "resolver=+0x%llX player_component_update=+0x%llX",
                           rva(g_natives.initialize),
                           rva(g_natives.direct),
                           rva(g_natives.factory),
                           rva(g_natives.transform),
                           rva(g_natives.resolver),
                           rva(update)));
    return true;
}

void reset_storage(PlacementStorage& storage) noexcept {
    storage = {};
    constexpr std::uint64_t zero = 0;
    constexpr std::uint64_t capacity = kPlacementPayloadBytes;
    constexpr std::uint64_t alignment = 0x10;
    std::memcpy(storage.bytes.data(), &zero, sizeof zero);
    std::memcpy(storage.bytes.data() + 0x10, &zero, sizeof zero);
    std::memcpy(storage.bytes.data() + 0x18, &kInvalidHandle, sizeof kInvalidHandle);
    std::memcpy(storage.bytes.data() + 0x20, &capacity, sizeof capacity);
    std::memcpy(storage.bytes.data() + 0x28, &alignment, sizeof alignment);
    std::memcpy(storage.bytes.data() + 0x30, &zero, sizeof zero);
}

/** @return The descriptor offset the initializer wrote at the storage base. */
[[nodiscard]] std::int64_t raw_descriptor_offset(const PlacementStorage& storage) noexcept {
    std::int64_t offset = 0;
    std::memcpy(&offset, storage.bytes.data(), sizeof offset);
    return offset;
}

/** @return True when the descriptor's transform lanes lie inside the storage. */
[[nodiscard]] constexpr bool descriptor_in_storage(std::int64_t offset) noexcept {
    constexpr auto limit = static_cast<std::int64_t>(kPlacementHeaderBytes + kPlacementPayloadBytes
                                                     - kDescriptorPosition - 0x10);
    return offset > 0 && offset <= limit;
}

// Native calls stay in these POD-only frames so each can carry its own SEH guard.

[[nodiscard]] bool call_resolver(std::uint32_t tag, const std::byte*& definition) noexcept {
    definition = nullptr;
    __try {
        definition = g_natives.resolver(tag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        definition = nullptr;
        return false;
    }
}

[[nodiscard]] bool read_type(const std::byte* definition, std::uint8_t& type) noexcept {
    type = 0;
    __try {
        type = static_cast<std::uint8_t>(definition[kDefinitionObjectType]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        type = 0;
        return false;
    }
}

[[nodiscard]] bool call_initialize(PlacementInitialize initialize, void* storage,
                                   std::uint32_t tag, std::uint8_t& result) noexcept {
    result = 0;
    __try {
        result = initialize(storage, tag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0;
        return false;
    }
}

/** SEH filter: copies the exception code and address, then runs the handler. */
int capture_fault(const EXCEPTION_POINTERS* info, Fault& fault) noexcept {
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        fault.code = info->ExceptionRecord->ExceptionCode;
        fault.address = info->ExceptionRecord->ExceptionAddress;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

[[nodiscard]] bool call_factory(void* descriptor, std::uint32_t& handle, std::uint32_t*& returned,
                                Fault& fault) noexcept {
    handle = kInvalidHandle;
    returned = nullptr;
    fault = {};
    __try {
        returned = g_natives.factory(&handle, descriptor);
        return true;
    } __except (capture_fault(GetExceptionInformation(), fault)) {
        return false;
    }
}

[[nodiscard]] bool call_transform(void* datum, const float* transform) noexcept {
    __try {
        g_natives.transform(datum, transform);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] bool read_updated_handle(const void* object, std::uint32_t& handle) noexcept {
    handle = kInvalidHandle;
    __try {
        std::memcpy(&handle, static_cast<const std::byte*>(object) + kUpdatedObjectHandle,
                    sizeof handle);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        handle = kInvalidHandle;
        return false;
    }
}

/** Sandbox teleport::is_controlled_object: same datum index as the local controlled handle. */
[[nodiscard]] bool is_controlled_object(const void* object) noexcept {
    std::uint32_t controlled = kInvalidHandle;
    std::uint32_t candidate = kInvalidHandle;
    return object != nullptr && teleport::controlled_handle(controlled)
           && read_updated_handle(object, candidate)
           && (controlled & kHandleIndexMask) == (candidate & kHandleIndexMask);
}

[[nodiscard]] bool live(std::uint32_t handle) noexcept {
    return handle != kInvalidHandle && world_objects::live_datum(handle) != nullptr;
}

/** Applies a queued transform once the created datum resolves, as the Sandbox does. */
void service_activation() noexcept {
    if (!g_activationQueued.load(std::memory_order_acquire)) {
        return;
    }
    std::byte* const datum = world_objects::live_datum(g_activation.handle);
    const bool applied = datum != nullptr && call_transform(datum, g_activation.transform.data());
    ++g_activation.attempts;
    if (!applied && g_activation.attempts < kActivationAttempts) {
        return;
    }
    std::array<char, 160> line{};
    log_line(line,
             std::snprintf(line.data(), line.size(),
                           "DEBUG_SAULO tower_npc_restore stage=activation handle=0x%08X datum=%p "
                           "attempts=%u result=%s",
                           g_activation.handle, static_cast<void*>(datum),
                           static_cast<unsigned>(g_activation.attempts),
                           applied ? "ok" : "fail"));
    g_activation = {};
    g_activationQueued.store(false, std::memory_order_release);
}

/** Creation descriptor fields proven at runtime (npc28): flags +0x68, aux payload offset +0x78. */
constexpr std::size_t kDescriptorFlags = 0x68;
constexpr std::size_t kDescriptorAux = 0x78;

/** What one spawn built, for the restoration and scenery logs; the spawn never reads it. */
struct SpawnReport {
    bool resolved{};
    bool resolverFault{};
    bool typeRead{};
    std::uint8_t type{};
    std::uint8_t primaryResult{};
    std::uint8_t directResult{};
    bool descriptorBuilt{};
    bool usedDirect{};
    bool flagsRead{};
    std::uint32_t descriptorFlags{};
    std::int64_t auxRelative{};
    bool factoryReached{};
    /** First failure: natives_unresolved, tag_not_resident, no_descriptor, fault, bad handle. */
    const char* failure{"none"};
};

/**
 * Sandbox spawn_one on the Shadowkeep image: resolve the definition, build a placement, write the
 * transform into the descriptor, then let the factory allocate and construct the object.
 * @param verbose Logs every stage; otherwise only the failures.
 */
[[nodiscard]] bool spawn_native_shadowkeep(std::uint32_t tag,
                                           const Transform& transform,
                                           std::uint32_t& handle,
                                           const char* label,
                                           SpawnReport* report = nullptr,
                                           bool verbose = true) noexcept {
    handle = kInvalidHandle;
    SpawnReport ignored{};
    SpawnReport& out = report != nullptr ? *report : ignored;
    out = {};
    if (!g_installed.load(std::memory_order_acquire)) {
        out.failure = "natives_unresolved";
        log_failure_as(label, "resolve", tag, "natives_unresolved");
        return false;
    }

    const std::byte* definition = nullptr;
    const bool resolverOk = call_resolver(tag, definition);
    std::uint8_t type = 0;
    const bool typeOk = definition != nullptr && read_type(definition, type);
    out.resolved = definition != nullptr;
    out.resolverFault = !resolverOk;
    out.typeRead = typeOk;
    out.type = type;
    // The allocate tracer finds a factory descriptor by this pointer, so it names the origin.
    world_objects::note_npc_definition(tag, reinterpret_cast<std::uintptr_t>(definition));
    std::array<char, 320> line{};
    if (verbose) {
        log_line(line,
                 std::snprintf(line.data(), line.size(),
                               "DEBUG_SAULO %s stage=resolve tag=0x%08X definition=%p "
                               "type=%u type_read=%u resolver_fault=%u",
                               label, tag, static_cast<const void*>(definition),
                               static_cast<unsigned>(type), static_cast<unsigned>(typeOk),
                               static_cast<unsigned>(!resolverOk)));
    }

    PlacementStorage storage{};
    reset_storage(storage);
    std::uint8_t primary = 0;
    const bool primaryOk =
        call_initialize(g_natives.initialize, storage.bytes.data(), tag, primary);
    std::uint8_t direct = 0;
    bool directOk = true;
    bool usedDirect = false;
    // The Sandbox falls back to the direct build only for a tag the resolver knows.
    if (primary == 0 && definition != nullptr) {
        reset_storage(storage);
        usedDirect = true;
        directOk = call_initialize(g_natives.direct, storage.bytes.data(), tag, direct);
    }
    const bool initialized = usedDirect ? direct != 0 : primary != 0;
    const std::int64_t offset = raw_descriptor_offset(storage);
    std::byte* const descriptor =
        initialized && descriptor_in_storage(offset) ? storage.bytes.data() + offset : nullptr;

    std::array<float, 4> rotation{};
    std::memcpy(rotation.data(), transform.data(), sizeof rotation);
    // Every NPC transform carries kSpawnScale here; scenery carries its authored scale.
    const std::array<float, 4> position{transform[4], transform[5], transform[6], transform[7]};
    out.primaryResult = primary;
    out.directResult = direct;
    out.usedDirect = usedDirect;
    if (verbose) {
        log_line(line,
                 std::snprintf(line.data(), line.size(),
                               "DEBUG_SAULO %s stage=placement primary=%u primary_fault=%u "
                               "direct_used=%u direct=%u direct_fault=%u offset=0x%llX "
                               "descriptor=%p pos=(%.8f,%.8f,%.8f) "
                               "rotation=(%.4f,%.4f,%.4f,%.4f) scale=%.2f",
                               label, static_cast<unsigned>(primary),
                               static_cast<unsigned>(!primaryOk),
                               static_cast<unsigned>(usedDirect), static_cast<unsigned>(direct),
                               static_cast<unsigned>(!directOk),
                               static_cast<unsigned long long>(offset),
                               static_cast<void*>(descriptor), position[0], position[1],
                               position[2], rotation[0], rotation[1], rotation[2], rotation[3],
                               position[3]));
    }
    if (descriptor == nullptr) {
        out.failure = definition == nullptr ? "tag_not_resident" : "no_descriptor";
        log_failure_as(label, "placement", tag, out.failure);
        return false;
    }
    std::memcpy(descriptor + kDescriptorRotation, rotation.data(), sizeof rotation);
    std::memcpy(descriptor + kDescriptorPosition, position.data(), sizeof position);
    out.descriptorBuilt = true;
    // Our own storage, so the read only needs the bound check.
    constexpr auto kAuxEnd = static_cast<std::int64_t>(kDescriptorAux + sizeof(std::int64_t));
    out.flagsRead = offset + kAuxEnd <= static_cast<std::int64_t>(storage.bytes.size());
    if (out.flagsRead) {
        std::memcpy(&out.descriptorFlags, descriptor + kDescriptorFlags,
                    sizeof out.descriptorFlags);
        std::memcpy(&out.auxRelative, descriptor + kDescriptorAux, sizeof out.auxRelative);
    }

    std::uint32_t* returned = nullptr;
    Fault fault{};
    out.factoryReached = true;
    g_factoryUsed = true;
    if (!call_factory(descriptor, handle, returned, fault)) {
        out.failure = "fault";
        // The handle the allocator wrote before the fault names an incomplete creation.
        const bool inside = in_game_image(fault.address);
        log_line(line,
                 std::snprintf(line.data(), line.size(),
                               "DEBUG_SAULO %s stage=factory result=fault "
                               "exception_code=0x%08lX exception_address=%p exception_rva=%s0x%llX "
                               "factory=+0x%llX descriptor=%p handle=0x%08X handle_state=partial",
                               label, static_cast<unsigned long>(fault.code), fault.address,
                               inside ? "+" : "outside_image:",
                               inside ? rva(fault.address)
                                      : static_cast<unsigned long long>(
                                            reinterpret_cast<std::uintptr_t>(fault.address)),
                               rva(g_natives.factory), static_cast<void*>(descriptor), handle));
        log_failure_as(label, "factory", tag, "fault");
        handle = kInvalidHandle;
        return false;
    }
    if (verbose) {
        log_line(line,
                 std::snprintf(line.data(), line.size(),
                               "DEBUG_SAULO %s stage=factory result=returned factory=+0x%llX "
                               "descriptor=%p out=%p returned=%p handle=0x%08X",
                               label, rva(g_natives.factory), static_cast<void*>(descriptor),
                               static_cast<void*>(&handle), static_cast<void*>(returned),
                               handle));
    }
    if (handle == kInvalidHandle) {
        out.failure = "invalid_handle";
        log_failure_as(label, "factory", tag, "invalid_handle");
        return false;
    }
    if (typeOk && needs_activation(type)) {
        g_activation = {};
        g_activation.handle = handle;
        std::memcpy(g_activation.transform.data(), rotation.data(), sizeof rotation);
        std::memcpy(g_activation.transform.data() + 4, position.data(), sizeof position);
        g_activationQueued.store(true, std::memory_order_release);
    }
    return true;
}

/** Why the Tower gate is closed, or ready. */
enum class TowerCondition : std::uint8_t {
    noSession,
    wrongActivity,
    notInWorld,
    ready,
};

/** Uses the joined activity and client-reported current bubble, not the arrival target. */
[[nodiscard]] TowerCondition tower_bubble(std::int32_t& bubble) noexcept {
    bubble = -1;
    const std::uint64_t session =
        state::activity::membership::live_region_session(state::activity::kAbsentSessionId);
    state::activity::SessionBinding binding{};
    if (session == state::activity::kAbsentSessionId
        || !state::activity::snapshot_binding(session, binding)) {
        return TowerCondition::noSession;
    }
    const auto& destination = binding.destination;
    if (destination.packageNameLength != kTowerActivity.size()
        || std::memcmp(destination.packageName.data(), kTowerActivity.data(),
                       kTowerActivity.size()) != 0) {
        return TowerCondition::wrongActivity;
    }
    const auto placement = state::activity::membership::reported_placement(session);
    if (!placement.clientInWorld
        || state::activity::membership::instantiated_region(placement) < 0) {
        return TowerCondition::notInWorld;
    }
    bubble = static_cast<std::int32_t>(placement.bubble);
    return TowerCondition::ready;
}

// Tower NPC factory restoration. Every NPC here comes back with the Tower itself from the object
// factory at its original package transform; no squad, roster or scenario state is touched.
// Yuna, Saladin and Xur are not here: tower_npc_authored_restoration brings each back as its own
// authored squad, and no factory path, production or debug, creates them.
constexpr bool kPermanentAutoSpawn = true;

/** One NPC with its package transform, kept as exact float bits. */
struct TowerNpc {
    const char* name;
    std::uint32_t tag;
    /** Tower bubble of the authored placement: 0 annex, 1 Bazaar, 6 court, 7 hangar. */
    std::int32_t bubble;
    std::array<std::uint32_t, 3> positionBits;
    std::array<std::uint32_t, 4> rotationBits;
    /** Package row the transform came from. */
    const char* source;
};

// Object actors: the row embedded in their object-slot config, matched by entity tag.
constexpr std::array kTowerNpcs{
    TowerNpc{"Saint14", 0x80FCDB40U, 7, {0x4334136BU, 0x428DBC6FU, 0x40C31C90U},
             {0U, 0U, 0xBF7EEF1FU, 0xBDBAB23AU}, "0x80B4A22B+0x580"},
    TowerNpc{"Drifter", 0x80FCC2F1U, 0, {0xC2E4399BU, 0x42747DB4U, 0xC1CAE283U},
             {0U, 0U, 0xBF800000U, 0xB33BBD2EU}, "0x80B4A1BE+0x580"},
    // o_penumbra_vendor, confirmed in game as Benedict-99/40.
    TowerNpc{"Benedict99", 0x80FCC2FEU, 0, {0xC3191576U, 0x42699274U, 0xC1E191AFU},
             {0U, 0U, 0x3E8E4073U, 0xBF75EBC3U}, "0x80B4A1C5+0x580"},
};
constexpr std::size_t kNpcCount = kTowerNpcs.size();
static_assert(kNpcCount <= 32);

[[nodiscard]] Transform original_transform(const TowerNpc& npc) noexcept {
    return {std::bit_cast<float>(npc.rotationBits[0]), std::bit_cast<float>(npc.rotationBits[1]),
            std::bit_cast<float>(npc.rotationBits[2]), std::bit_cast<float>(npc.rotationBits[3]),
            std::bit_cast<float>(npc.positionBits[0]), std::bit_cast<float>(npc.positionBits[1]),
            std::bit_cast<float>(npc.positionBits[2]), kSpawnScale};
}

/** Production restoration state; only the controlled update service touches it. */
struct Restoration {
    /** The production copy Sunrise created: origin sunrise_factory. */
    std::uint32_t handle{kInvalidHandle};
    std::uint64_t nextAttempt{};
    /** Last one-shot skip, so a repeated skip logs once. */
    const char* lastReason{};
    std::uint32_t lastHandle{kInvalidHandle};
};
std::array<Restoration, kNpcCount> g_restoration{};
/** Tower session the production handles belong to; a new one forgets them. */
std::uint64_t g_restorationSession{state::activity::kAbsentSessionId};

void log_restore(const TowerNpc& npc, const char* stage, const char* detail) noexcept {
    std::array<char, 320> line{};
    log_line(line, std::snprintf(line.data(), line.size(),
                                 "DEBUG_SAULO tower_npc_restore name=%s stage=%s tag=0x%08X %s",
                                 npc.name, stage, npc.tag, detail));
}

/**
 * Duplicate guard: any live copy of this NPC. Production is additive, so whoever owns such a copy
 * (the game, or Sunrise before a world change) keeps it, and nothing here destroys it.
 */
[[nodiscard]] bool live_production_copy(const TowerNpc& npc,
                                        world_objects::NpcCopy& found) noexcept {
    std::array<world_objects::NpcCopy, 8> copies{};
    if (world_objects::live_npc_copies(npc.tag, copies) == 0) {
        return false;
    }
    found = copies[0];
    return true;
}

/** Logs a skip; an automatic one only when its reason or handle changes. */
void note_skip(std::size_t index, bool automatic, const char* reason, std::uint32_t handle,
               const char* detail) noexcept {
    Restoration& state = g_restoration[index];
    if (automatic) {
        if (state.lastReason == reason && state.lastHandle == handle) {
            return;
        }
        state.lastReason = reason;
        state.lastHandle = handle;
    }
    log_restore(kTowerNpcs[index], "skip", detail);
}

/**
 * The one production spawner: the NPC's own bubble, resident tag, no live copy, then the factory
 * at the original transform.
 */
[[nodiscard]] bool spawn_production(std::size_t index, const char* trigger,
                                    std::int32_t bubble) noexcept {
    const TowerNpc& npc = kTowerNpcs[index];
    Restoration& state = g_restoration[index];
    const bool automatic = std::strcmp(trigger, "auto") == 0;
    std::array<char, 96> request{};
    (void)std::snprintf(request.data(), request.size(), "trigger=%s bubble=%d current_bubble=%d",
                        trigger, npc.bubble, bubble);
    // A click always reports its request; the automatic pass, which runs these guards on every
    // update, reports only the attempts that reach the factory.
    if (!automatic) {
        log_restore(npc, "request", request.data());
    }
    std::array<char, 224> detail{};
    if (live(state.handle)) {
        (void)std::snprintf(detail.data(), detail.size(),
                            "reason=already_live handle=0x%08X origin=sunrise_factory trigger=%s",
                            state.handle, trigger);
        note_skip(index, automatic, "already_live", state.handle, detail.data());
        return false;
    }
    // The original transform belongs to the NPC's own bubble; elsewhere it may not be streamed.
    if (bubble != npc.bubble) {
        (void)std::snprintf(detail.data(), detail.size(),
                            "reason=wrong_bubble handle=0x%08X trigger=%s bubble=%d "
                            "current_bubble=%d",
                            kInvalidHandle, trigger, npc.bubble, bubble);
        note_skip(index, automatic, "wrong_bubble", kInvalidHandle, detail.data());
        return false;
    }
    // A tag the current bubble has not streamed is a result, not something to force.
    const std::byte* definition = nullptr;
    if (!call_resolver(npc.tag, definition) || definition == nullptr) {
        (void)std::snprintf(detail.data(), detail.size(),
                            "reason=not_resident handle=0x%08X trigger=%s", kInvalidHandle,
                            trigger);
        note_skip(index, automatic, "not_resident", kInvalidHandle, detail.data());
        return false;
    }
    world_objects::note_npc_definition(npc.tag, reinterpret_cast<std::uintptr_t>(definition));
    world_objects::NpcCopy copy{};
    if (live_production_copy(npc, copy)) {
        // A sunrise_factory copy is a production copy this state lost track of; any other origin
        // is the game's own actor. Neither is ever destroyed.
        const bool gameOwned =
            copy.origin != nullptr && std::strcmp(copy.origin, "sunrise_factory") != 0;
        const char* const reason = gameOwned ? "existing_game_owned" : "already_live";
        (void)std::snprintf(detail.data(), detail.size(),
                            "reason=%s handle=0x%08X origin=%s trigger=%s", reason, copy.handle,
                            copy.origin != nullptr ? copy.origin : "unknown", trigger);
        note_skip(index, automatic, reason, copy.handle, detail.data());
        return false;
    }
    if (automatic) {
        log_restore(npc, "request", request.data());
    }
    std::array<char, 64> label{};
    (void)std::snprintf(label.data(), label.size(), "tower_npc_spawn name=%s source=official",
                        npc.name);
    std::uint32_t handle = kInvalidHandle;
    SpawnReport report{};
    std::array<char, 320> line{};
    if (!spawn_native_shadowkeep(npc.tag, original_transform(npc), handle, label.data(),
                                 &report)) {
        log_line(line, std::snprintf(line.data(), line.size(),
                                     "DEBUG_SAULO %s stage=factory handle=0x%08X result=failed "
                                     "reason=%s%s trigger=%s",
                                     label.data(), kInvalidHandle,
                                     report.factoryReached ? "" : "not_reached_", report.failure,
                                     trigger));
        return false;
    }
    state.handle = handle;
    state.lastReason = nullptr;
    state.lastHandle = kInvalidHandle;
    log_line(line, std::snprintf(line.data(), line.size(),
                                 "DEBUG_SAULO %s stage=factory handle=0x%08X result=created "
                                 "origin=sunrise_factory trigger=%s kind=%s bubble=%d row=%s",
                                 label.data(), handle, trigger, "permanent", npc.bubble,
                                 npc.source));
    return true;
}

/** Forgets every production handle; the game tears its own world down, nothing is destroyed. */
void clear_production(const char* reason) noexcept {
    for (std::size_t index = 0; index < kNpcCount; ++index) {
        Restoration& state = g_restoration[index];
        if (state.handle != kInvalidHandle) {
            std::array<char, 96> detail{};
            (void)std::snprintf(detail.data(), detail.size(), "reason=%s handle=0x%08X", reason,
                                state.handle);
            log_restore(kTowerNpcs[index], "clear", detail.data());
        }
        state = {};
    }
}

/**
 * Automatic restoration: Tower, the NPC's own bubble, resident tag, no live copy. At most one
 * factory call per update, as the Sandbox does, and one attempt per NPC every kAutoRetryMs.
 */
void service_restoration(TowerCondition condition, std::int32_t bubble) noexcept {
    const std::uint64_t session =
        state::activity::membership::live_region_session(state::activity::kAbsentSessionId);
    if (condition != TowerCondition::ready) {
        if (g_restorationSession != state::activity::kAbsentSessionId) {
            clear_production("world_change");
            g_restorationSession = state::activity::kAbsentSessionId;
        }
        return;
    }
    if (session != g_restorationSession) {
        if (g_restorationSession != state::activity::kAbsentSessionId) {
            clear_production("generation_change");
        }
        g_restorationSession = session;
    }
    const std::uint64_t now = GetTickCount64();
    for (std::size_t index = 0; index < kNpcCount; ++index) {
        const TowerNpc& npc = kTowerNpcs[index];
        Restoration& state = g_restoration[index];
        if (state.handle != kInvalidHandle && !live(state.handle)) {
            std::array<char, 96> detail{};
            (void)std::snprintf(detail.data(), detail.size(), "reason=handle_invalid handle=0x%08X",
                                state.handle);
            log_restore(npc, "rearm", detail.data());
            state.handle = kInvalidHandle;
            state.lastReason = nullptr;
            state.lastHandle = kInvalidHandle;
        }
        if (!kPermanentAutoSpawn || npc.bubble != bubble) {
            // Leaving the bubble only stops creation there and re-arms the one-shot skip notes.
            state.lastReason = nullptr;
            state.lastHandle = kInvalidHandle;
            continue;
        }
        if (live(state.handle)) {
            (void)spawn_production(index, "auto", bubble);
            continue;
        }
        if (now < state.nextAttempt) {
            continue;
        }
        state.nextAttempt = now + kAutoRetryMs;
        if (spawn_production(index, "auto", bubble)) {
            return;
        }
    }
}

// Static scenery the factory recreates from a package static container, each placement at its
// authored transform: Saint-14's container in the Hangar, once per session, and Yuna's props in
// the Bazaar while Yuna's toggle is on. No carrier, roster, squad or Activity Host is involved.
// Only Yuna's props are ever destroyed, and only the copies Sunrise created.
namespace package_reader = ::sunrise::middleware::content::packages::reader;
namespace package_tables = ::sunrise::middleware::content::packages::tables;

constexpr std::uint32_t kSaintEntity = 0x80FCDB40U;
constexpr std::int32_t kHangarBubble = 7;
constexpr std::size_t kSceneCapacity = 256;

/** One static container the factory recreates. */
struct SceneSpec {
    /** Log prefix of the container's lines. */
    const char* log;
    std::uint32_t container;
    /** One line per entry and per factory call; the props log their summaries only. */
    bool verbose;
};
/** Saint-14's encounter (0x80B4A232, config 0x80B4A22B) static container. */
constexpr SceneSpec kSaintScene{"saint_scene", 0x80B4AF28U, true};
/** enc_bazaar_igr_korea_props (encounter 0x80B4A537): 43 placements of 19 entities. */
constexpr SceneSpec kYunaProps{"yuna_scene", 0x80B4A530U, false};

/** One container placement, with its entry index so the native registry can be asked about it. */
struct ScenePlacement {
    std::uint32_t index{};
    std::uint32_t entity{};
    Transform transform{};
};

/** A container's placements, read once per process: package data does not change. */
struct SceneList {
    std::array<ScenePlacement, kSceneCapacity> entries{};
    std::size_t count{};
    std::uint64_t declared{};
    std::uint32_t unreadable{};
    bool loaded{};
};
SceneList g_saintSceneList{};
SceneList g_yunaPropsList{};

/** @return One pass's handle table with every entry absent. */
[[nodiscard]] constexpr std::array<std::uint32_t, kSceneCapacity> no_handles() noexcept {
    std::array<std::uint32_t, kSceneCapacity> handles{};
    for (std::uint32_t& handle : handles) {
        handle = kInvalidHandle;
    }
    return handles;
}

/** One pass over a list; only the controlled update service touches it. */
struct SceneryState {
    std::uint64_t session{state::activity::kAbsentSessionId};
    bool active{};
    /** The automatic pass ran in this Tower session (Saint-14's scenery). */
    bool done{};
    bool pausedLogged{};
    std::size_t cursor{};
    const char* trigger{"auto"};
    std::uint32_t created{};
    std::uint32_t skipped{};
    /** Entries skipped because the game placed them itself. */
    std::uint32_t native{};
    std::uint32_t failed{};
    /** Sunrise's copy of each entry. */
    std::array<std::uint32_t, kSceneCapacity> handles{no_handles()};
};
SceneryState g_saintScenery{};

void log_scene(const SceneSpec& spec, const char* stage, const char* detail) noexcept {
    std::array<char, 320> line{};
    log_line(line, std::snprintf(line.data(), line.size(),
                                 "DEBUG_SAULO %s stage=%s container=0x%08X %s", spec.log, stage,
                                 spec.container, detail));
}

/** Forgets a pass and every handle; the world owns the objects from here on. */
void reset_scenery(SceneryState& pass, std::uint64_t session) noexcept {
    pass = {};
    pass.session = session;
    pass.handles.fill(kInvalidHandle);
}

/** Reads one static container through Sunrise's package reader, as the activity SDK passes do. */
[[nodiscard]] bool load_scene(const SceneSpec& spec, SceneList& list) noexcept {
    if (list.loaded) {
        return true;
    }
    // Several megabytes of reader storage, so it lives outside the stack.
    static package_reader::Scratch scratch{};
    static std::vector<std::byte> blob{};
    package_reader::BlockKeys keys{};
    core::path::Buffer directory{};
    const char* failure = nullptr;
    std::uint32_t classId = 0;
    if (!content::items::packages::collect_keys(keys)) {
        failure = "keys_unavailable";
    } else if (!content::items::packages::package_directory(directory)) {
        failure = "package_directory_unavailable";
    } else {
        const package_reader::Source source{directory.chars.data(), &keys};
        if (!package_reader::read_tag(source, scratch, spec.container, blob, classId)) {
            failure = "read_failed";
        }
        package_reader::close_files(scratch);
    }
    SecureZeroMemory(&keys, sizeof keys);
    package_tables::Array array{};
    if (failure == nullptr && classId != package_tables::kAuthoredPlacementListClass) {
        failure = "class_mismatch";
    } else if (failure == nullptr && !package_tables::authored_placements(blob, array)) {
        failure = "no_placement_array";
    }
    std::array<char, 160> detail{};
    if (failure != nullptr) {
        (void)std::snprintf(detail.data(), detail.size(), "result=failed reason=%s class=0x%08X",
                            failure, classId);
        log_scene(spec, "read", detail.data());
        return false;
    }
    list = {};
    list.declared = array.count;
    for (std::size_t index = 0; index < array.count && list.count < kSceneCapacity; ++index) {
        package_tables::AuthoredPlacement placement{};
        if (!package_tables::authored_placement_at(blob, array, index, placement)) {
            ++list.unreadable;
            continue;
        }
        const float scale = std::isfinite(placement.uniformScale) && placement.uniformScale > 0.0F
                                ? placement.uniformScale
                                : kSpawnScale;
        ScenePlacement& entry = list.entries[list.count++];
        entry.index = static_cast<std::uint32_t>(index);
        entry.entity = placement.classListTag;
        entry.transform = {placement.rotation[0], placement.rotation[1], placement.rotation[2],
                           placement.rotation[3], placement.position[0], placement.position[1],
                           placement.position[2], scale};
    }
    list.loaded = true;
    (void)std::snprintf(detail.data(), detail.size(),
                        "result=ok class=0x%08X declared=%llu placements=%zu unreadable=%u "
                        "truncated=%u",
                        classId, static_cast<unsigned long long>(list.declared), list.count,
                        list.unreadable, list.declared > kSceneCapacity ? 1U : 0U);
    log_scene(spec, "read", detail.data());
    return true;
}

/** @return True for Saint-14 or any other Tower NPC actor, which the scenery must never create. */
[[nodiscard]] bool tower_npc_entity(std::uint32_t entity) noexcept {
    if (entity == kSaintEntity || tower_npc_authored_restoration::authored_entity(entity)) {
        return true;
    }
    for (const TowerNpc& npc : kTowerNpcs) {
        if (npc.tag == entity) {
            return true;
        }
    }
    return false;
}

/** Starts a pass over its list from the first entry. @return False when the list cannot load. */
bool start_scenery(const SceneSpec& spec, SceneList& list, SceneryState& pass,
                   const char* trigger, std::int32_t bubble) noexcept {
    if (spec.verbose) {
        std::array<char, 96> detail{};
        (void)std::snprintf(detail.data(), detail.size(), "trigger=%s bubble=%d", trigger,
                            bubble);
        log_scene(spec, "request", detail.data());
    }
    pass.done = true;
    if (!load_scene(spec, list)) {
        log_scene(spec, "summary", "placements=0 created=0 skipped=0 failed=0 result=read_failed");
        return false;
    }
    pass.active = true;
    pass.pausedLogged = false;
    pass.cursor = 0;
    pass.trigger = trigger;
    pass.created = 0;
    pass.skipped = 0;
    pass.native = 0;
    pass.failed = 0;
    return true;
}

/**
 * Walks a pass: skips Tower NPC actors, entries the game already placed and entries with a live
 * Sunrise copy, and creates the rest. At most one factory call per update, none while another
 * spawn's activation is still pending.
 * @return True once the pass reached the end of its list.
 */
[[nodiscard]] bool step_scenery(const SceneSpec& spec, const SceneList& list,
                                SceneryState& pass) noexcept {
    if (g_factoryUsed || g_activationQueued.load(std::memory_order_acquire)) {
        return false;
    }
    std::array<char, 192> detail{};
    while (pass.cursor < list.count) {
        const std::size_t slot = pass.cursor++;
        const ScenePlacement& entry = list.entries[slot];
        std::array<world_objects::Instance, 1> probe{};
        const char* skip = nullptr;
        if (tower_npc_entity(entry.entity)) {
            skip = "tower_npc_actor";
        } else if (world_objects::find(spec.container, entry.index, probe) != 0) {
            skip = "native_live";
            ++pass.native;
        } else if (live(pass.handles[slot])) {
            skip = "already_live";
        }
        if (skip != nullptr) {
            ++pass.skipped;
            if (spec.verbose) {
                (void)std::snprintf(detail.data(), detail.size(),
                                    "entry=%u entity=0x%08X handle=0x%08X result=skipped "
                                    "reason=%s",
                                    entry.index, entry.entity, pass.handles[slot], skip);
                log_scene(spec, "entry", detail.data());
            }
            continue;
        }
        std::array<char, 64> label{};
        (void)std::snprintf(label.data(), label.size(), "%s entry=%u", spec.log, entry.index);
        std::uint32_t handle = kInvalidHandle;
        SpawnReport report{};
        const bool created = spawn_native_shadowkeep(entry.entity, entry.transform, handle,
                                                     label.data(), &report, spec.verbose);
        pass.handles[slot] = created ? handle : kInvalidHandle;
        if (created) {
            ++pass.created;
        } else {
            ++pass.failed;
        }
        if (spec.verbose || !created) {
            (void)std::snprintf(detail.data(), detail.size(),
                                "entry=%u entity=0x%08X type=%d handle=0x%08X result=%s reason=%s "
                                "pos=(%.3f,%.3f,%.3f) scale=%.2f",
                                entry.index, entry.entity, report.typeRead ? report.type : -1,
                                pass.handles[slot], created ? "created" : "failed", report.failure,
                                entry.transform[4], entry.transform[5], entry.transform[6],
                                entry.transform[7]);
            log_scene(spec, "entry", detail.data());
        }
        // One factory call per update, as the Sandbox does.
        if (pass.cursor < list.count) {
            return false;
        }
    }
    return true;
}

/** Saint-14's scenery: once per Tower session, the first time the player is in the Hangar. */
void service_saint_scenery(TowerCondition condition, std::int32_t bubble) noexcept {
    const std::uint64_t session =
        state::activity::membership::live_region_session(state::activity::kAbsentSessionId);
    SceneryState& pass = g_saintScenery;
    std::array<char, 192> detail{};
    // Leaving the Tower or a new session forgets the pass; nothing is destroyed.
    const std::uint64_t current =
        condition == TowerCondition::ready ? session : state::activity::kAbsentSessionId;
    if (current != pass.session) {
        if (pass.session != state::activity::kAbsentSessionId && (pass.done || pass.active)) {
            (void)std::snprintf(detail.data(), detail.size(), "reason=%s created=%u",
                                current == state::activity::kAbsentSessionId
                                    ? "world_change"
                                    : "generation_change",
                                pass.created);
            log_scene(kSaintScene, "clear", detail.data());
        }
        reset_scenery(pass, current);
    }
    if (condition != TowerCondition::ready) {
        return;
    }
    if (!pass.active && !pass.done && bubble == kHangarBubble) {
        (void)start_scenery(kSaintScene, g_saintSceneList, pass, "auto", bubble);
    }
    if (!pass.active) {
        return;
    }
    // Package residency follows the bubble, so the pass waits in the Hangar.
    if (bubble != kHangarBubble) {
        if (!pass.pausedLogged) {
            pass.pausedLogged = true;
            (void)std::snprintf(detail.data(), detail.size(),
                                "result=paused reason=not_hangar bubble=%d cursor=%zu", bubble,
                                pass.cursor);
            log_scene(kSaintScene, "wait", detail.data());
        }
        return;
    }
    pass.pausedLogged = false;
    if (!step_scenery(kSaintScene, g_saintSceneList, pass)) {
        return;
    }
    pass.active = false;
    (void)std::snprintf(detail.data(), detail.size(),
                        "placements=%zu created=%u skipped=%u failed=%u unreadable=%u trigger=%s",
                        g_saintSceneList.count, pass.created, pass.skipped, pass.failed,
                        g_saintSceneList.unreadable, pass.trigger);
    log_scene(kSaintScene, "summary", detail.data());
}

/** Yuna's props: the pass, whether it ran in this Bazaar visit, and any clear still settling. */
struct YunaProps {
    SceneryState pass{};
    bool visited{};
    bool disabledLogged{};
    /** Nonzero while destroyed copies get their grace before the sink check. */
    std::uint64_t clearDeadline{};
};
YunaProps g_yunaProps{};
/** Time a destroyed prop gets to disappear before the Sandbox "kill" moves it away. */
constexpr std::uint64_t kDestroyGraceMs = 2500;
/** Sandbox kill depth: an owned copy that survives its destroy is moved this far down. */
constexpr float kSinkDepth = 10000.0F;

/** Counts Sunrise's prop copies, and how many of them are live. */
void count_props(std::uint32_t& owned, std::uint32_t& living) noexcept {
    owned = 0;
    living = 0;
    for (const std::uint32_t handle : g_yunaProps.pass.handles) {
        owned += handle != kInvalidHandle ? 1U : 0U;
        living += live(handle) ? 1U : 0U;
    }
}

/** Destroys only Sunrise's live prop copies; the sink check follows after the grace. */
void clear_yuna_props(const char* reason, std::uint64_t now) noexcept {
    std::uint32_t owned = 0;
    std::uint32_t living = 0;
    count_props(owned, living);
    std::uint32_t destroyed = 0;
    for (const std::uint32_t handle : g_yunaProps.pass.handles) {
        if (live(handle) && world_objects::logical_destroy_object(handle)) {
            ++destroyed;
        }
    }
    std::array<char, 160> detail{};
    (void)std::snprintf(detail.data(), detail.size(),
                        "reason=%s owned=%u live=%u destroyed=%u", reason, owned, living,
                        destroyed);
    log_scene(kYunaProps, "clear", detail.data());
    g_yunaProps.pass.active = false;
    g_yunaProps.visited = false;
    g_yunaProps.clearDeadline = living != 0 ? now + kDestroyGraceMs : 0;
    if (g_yunaProps.clearDeadline == 0) {
        reset_scenery(g_yunaProps.pass, g_yunaProps.pass.session);
    }
}

/**
 * After the grace, a prop copy that outlived its destroy is moved far below the world through
 * ObjectTransform, the Sandbox's own way of removing its spawns. Then the tracking is forgotten.
 */
void settle_yuna_props(std::uint64_t now) noexcept {
    if (g_yunaProps.clearDeadline == 0 || now < g_yunaProps.clearDeadline) {
        return;
    }
    std::uint32_t sunk = 0;
    for (std::size_t slot = 0; slot < g_yunaPropsList.count; ++slot) {
        std::byte* const datum = world_objects::live_datum(g_yunaProps.pass.handles[slot]);
        if (datum == nullptr) {
            continue;
        }
        Transform below = g_yunaPropsList.entries[slot].transform;
        below[6] -= kSinkDepth;
        sunk += call_transform(datum, below.data()) ? 1U : 0U;
    }
    std::array<char, 96> detail{};
    (void)std::snprintf(detail.data(), detail.size(), "reason=settled still_live_sunk=%u", sunk);
    log_scene(kYunaProps, "clear", detail.data());
    g_yunaProps.clearDeadline = 0;
    reset_scenery(g_yunaProps.pass, g_yunaProps.pass.session);
}

/**
 * Yuna's props follow her toggle: on, each Bazaar visit creates the entries missing a live copy;
 * off, only Sunrise's copies are destroyed. A new session forgets them, as the world does.
 */
void service_yuna_props(TowerCondition condition, std::int32_t bubble) noexcept {
    const std::uint64_t now = GetTickCount64();
    const std::uint64_t current =
        condition == TowerCondition::ready
            ? state::activity::membership::live_region_session(state::activity::kAbsentSessionId)
            : state::activity::kAbsentSessionId;
    SceneryState& pass = g_yunaProps.pass;
    std::array<char, 192> detail{};
    if (current != pass.session) {
        // The world that held the copies is gone; nothing is left to destroy.
        const bool disabledLogged = g_yunaProps.disabledLogged;
        g_yunaProps = {};
        g_yunaProps.disabledLogged = disabledLogged;
        reset_scenery(pass, current);
    }
    settle_yuna_props(now);
    namespace authored = tower_npc_authored_restoration;
    if (!authored::authored_enabled(authored::Npc::yuna)) {
        std::uint32_t owned = 0;
        std::uint32_t living = 0;
        count_props(owned, living);
        if (living != 0 && g_yunaProps.clearDeadline == 0) {
            clear_yuna_props("toggle_off", now);
        } else if (!g_yunaProps.disabledLogged) {
            (void)std::snprintf(detail.data(), detail.size(), "reason=disabled owned=%u live=%u",
                                owned, living);
            log_scene(kYunaProps, "skip", detail.data());
        }
        g_yunaProps.disabledLogged = true;
        g_yunaProps.visited = false;
        pass.active = false;
        return;
    }
    g_yunaProps.disabledLogged = false;
    if (condition != TowerCondition::ready || g_yunaProps.clearDeadline != 0) {
        return;
    }
    if (bubble != kBazaarBubble) {
        // Leaving pauses the pass; the next visit recreates whatever the world dropped.
        g_yunaProps.visited = false;
        pass.active = false;
        return;
    }
    if (!g_yunaProps.visited) {
        g_yunaProps.visited = true;
        (void)start_scenery(kYunaProps, g_yunaPropsList, pass, "auto", bubble);
    }
    if (!pass.active || !step_scenery(kYunaProps, g_yunaPropsList, pass)) {
        return;
    }
    pass.active = false;
    std::uint32_t owned = 0;
    std::uint32_t living = 0;
    count_props(owned, living);
    (void)std::snprintf(detail.data(), detail.size(),
                        "placements=%zu created=%u native=%u skipped=%u failed=%u owned=%u "
                        "live=%u",
                        g_yunaPropsList.count, pass.created, pass.native, pass.skipped,
                        pass.failed, owned, living);
    log_scene(kYunaProps, "spawn", detail.data());
}

/** Sandbox PlayerComponentUpdate: original first, then the controlled Tower NPC service. */
void __fastcall player_component_update(void* object, void* input, void* authored) noexcept {
    const auto next = reinterpret_cast<PlayerComponentUpdate>(g_updateHook.original);
    if (next != nullptr) {
        next(object, input, authored);
    }
    if (!is_controlled_object(object) || g_servicing.test_and_set(std::memory_order_acquire)) {
        return;
    }
    service_activation();
    g_factoryUsed = false;
    // The live-copy guard and every handle check read the world-object registry.
    if (g_installed.load(std::memory_order_acquire) && world_objects::is_installed()) {
        std::int32_t bubble = -1;
        const TowerCondition condition = tower_bubble(bubble);
        service_restoration(condition, bubble);
        service_saint_scenery(condition, bubble);
        service_yuna_props(condition, bubble);
        // Hawthorne and Amanda wake-up, then Yuna, Saladin and Xur, each in its own bubble.
        tower_npc_authored_restoration::service(bubble);
    }
    g_servicing.clear(std::memory_order_release);
}

} // namespace

bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* update = nullptr;
    if (!resolve_natives(update)) {
        return false;
    }
    if (!hooking::detour::install({update, reinterpret_cast<void*>(&player_component_update)},
                                  g_updateHook)) {
        g_updateHook = {};
        log_failure_as("tower_npc_restore", "install", 0, "player_component_update_attach");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    return true;
}

void uninstall() noexcept {
    g_installed.store(false, std::memory_order_release);
    (void)hooking::detour::uninstall(g_updateHook);
    g_updateHook = {};
    g_natives = {};
    g_activation = {};
    g_activationQueued.store(false, std::memory_order_release);
    g_restoration = {};
    g_restorationSession = state::activity::kAbsentSessionId;
    reset_scenery(g_saintScenery, state::activity::kAbsentSessionId);
    // Forgets the props; the world that holds them owns them from here on.
    g_yunaProps = {};
    reset_scenery(g_yunaProps.pass, state::activity::kAbsentSessionId);
    tower_npc_authored_restoration::reset();
}

} // namespace sunrise::client::hooks::entity_spawn_test

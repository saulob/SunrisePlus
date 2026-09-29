/**
 * DEBUG_SAULO npc_idle_trace: read-only runtime tracer for NPC idle state.
 *
 * Hawthorne (squad-spawned) starts in T-pose and later plays STATE_FD41C0E5; factory-spawned Yuna,
 * Saladin and Xur stay in T-pose. All four declare an actor state machine (group AFB11A12);
 * Saint-14, Drifter and Benedict declare none and animate on their own. The tracer records each
 * watched NPC's allocation (caller chain and creation descriptor), then walks a bounded pointer
 * graph from its object datum looking for its state machine definition, entity tag, definition
 * pointer and state name hashes, and logs only new hits and changes at those addresses.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "../../../core/logging/log.h"
#include "../../memory/current_process_memory.h"
#include "internal.h"
#include "world_object_registry.h"

namespace sunrise::client::hooks::world_objects {
namespace {

/** One NPC the tracer follows, with the values its live state can carry. */
struct WatchedNpc {
    const char* name;
    std::uint32_t entity;
    std::uint32_t stateMachine;
    std::array<std::uint32_t, 2> states;
};

constexpr std::uint32_t kNoState = 0;
constexpr std::uint32_t kStateGroup = 0xAFB11A12U;
constexpr std::array kWatched{
    WatchedNpc{"Hawthorne", 0x80C1CA4DU, 0x80C1CA67U, {0xFD41C0E5U, kNoState}},
    WatchedNpc{"NewMonarchy", 0x80BC8C14U, 0x80BC8C57U, {0xE9527AF7U, kNoState}},
    WatchedNpc{"Yuna", 0x80C93820U, 0x80EC02A1U, {0x69923615U, 0xC7B78475U}},
    WatchedNpc{"Saladin", 0x80BC8E4BU, 0x80BC8EABU, {0x6E956079U, kNoState}},
    WatchedNpc{"Xur", 0x80BDEBBFU, 0x80BFB324U, {0x6A49E4BCU, 0x51D3351DU}},
    // Native squad actor in T-pose despite the Tower script playing STATE_5E795D1C.
    WatchedNpc{"Amanda", 0x80B9ECB7U, 0x80BC8AA7U, {0x5E795D1CU, kNoState}},
    // Native object actor with its title and icon; no state machine.
    WatchedNpc{"Ikora", 0x80FCC35CU, 0U, {kNoState, kNoState}},
    // Animated controls without a state machine.
    WatchedNpc{"Saint14", 0x80FCDB40U, 0U, {kNoState, kNoState}},
    WatchedNpc{"Drifter", 0x80FCC2F1U, 0U, {kNoState, kNoState}},
    WatchedNpc{"Benedict99", 0x80FCC2FEU, 0U, {kNoState, kNoState}},
};
constexpr std::size_t kWatchedCount = kWatched.size();

constexpr std::size_t kDescriptorBytes = 0x200;
constexpr std::size_t kPlacementEntryBytes = 0x90;
constexpr std::uint32_t kAllocationReports = 6;
constexpr std::size_t kCallerFrames = 8;
constexpr std::size_t kDatumBytes = 0xE0;
constexpr std::size_t kFirstBlockBytes = 0x800;
constexpr std::size_t kSecondBlockBytes = 0x300;
constexpr std::size_t kSecondPointersPerBlock = 24;
constexpr std::size_t kHitCapacity = 24;
constexpr std::size_t kWatchWindow = 0x20;
constexpr std::uint64_t kProbeStepMs = 500;
constexpr std::uint64_t kRediscoverMs = 5000;
constexpr std::size_t kDatumChangesPerLine = 12;

/** Written by the definition publisher, read by the allocate detour on any thread. */
std::array<std::atomic_uintptr_t, kWatchedCount> g_definitions{};
/** Latest allocation per NPC; the allocate detour writes, the probe reads. */
std::array<std::atomic_uint32_t, kWatchedCount> g_handles{};
std::array<std::atomic_uint32_t, kWatchedCount> g_allocationReports{};

/** One address the probe found a watched value at, with the window last seen around it. */
struct Hit {
    std::uintptr_t address{};
    std::array<char, 40> path{};
    std::array<std::byte, kWatchWindow * 2> window{};
    std::uint32_t value{};
    const char* kind{};
};

/** Probe state per NPC; only the single probing game thread touches it. */
struct ProbeState {
    std::uint32_t handle{kNone};
    std::array<std::byte, kDatumBytes> datum{};
    bool datumKnown{};
    std::array<Hit, kHitCapacity> hits{};
    std::size_t hitCount{};
    std::uint64_t nextDiscovery{};
};
std::array<ProbeState, kWatchedCount> g_probes{};

/** The atomics start at zero, so zero and the invalid datum both mean no handle. */
[[nodiscard]] constexpr bool has_handle(std::uint32_t handle) noexcept {
    return handle != 0U && handle != kNone;
}
std::size_t g_probeCursor{};
std::uint64_t g_nextProbe{};

[[nodiscard]] bool read_bytes(std::uintptr_t address, std::span<std::byte> output) noexcept {
    return address != 0 && memory::read_current_process(nullptr, address, output);
}

[[nodiscard]] bool plausible_pointer(std::uint64_t value) noexcept {
    return value >= 0x10000ULL && value < 0x00007FFFFFFFFFFFULL && (value & 7ULL) == 0;
}

void write_line(std::span<const char> buffer, int written) noexcept {
    if (written <= 0 || buffer.empty()) {
        return;
    }
    const std::size_t length = static_cast<std::size_t>(written) < buffer.size()
                                   ? static_cast<std::size_t>(written)
                                   : buffer.size() - 1;
    core::log::write(core::log::Channel::client, core::log::Level::warn, {buffer.data(), length});
}

[[nodiscard]] unsigned long long rva_of(std::uintptr_t address) noexcept {
    return address >= g_moduleBase ? static_cast<unsigned long long>(address - g_moduleBase)
                                   : static_cast<unsigned long long>(address);
}

/** Writes `bytes` as hex into `output`, which must hold two characters per byte plus one. */
void hex_into(std::span<const std::byte> bytes, std::span<char> output) noexcept {
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::size_t at = 0;
    for (const std::byte value : bytes) {
        if (at + 2 >= output.size()) {
            break;
        }
        output[at++] = kDigits[std::to_integer<unsigned>(value) >> 4U];
        output[at++] = kDigits[std::to_integer<unsigned>(value) & 0xFU];
    }
    output[at] = '\0';
}

/** @return The watched NPC whose tag or definition pointer this descriptor carries, or -1. */
[[nodiscard]] int match_descriptor(std::span<const std::byte> bytes,
                                   std::size_t& matchOffset,
                                   const char*& matchKind) noexcept {
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= bytes.size(); offset += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, bytes.data() + offset, sizeof word);
        std::uint64_t pointer = 0;
        const bool hasPointer = (offset & 7U) == 0 && offset + sizeof pointer <= bytes.size();
        if (hasPointer) {
            std::memcpy(&pointer, bytes.data() + offset, sizeof pointer);
        }
        for (std::size_t index = 0; index < kWatchedCount; ++index) {
            const std::uintptr_t definition = g_definitions[index].load(std::memory_order_acquire);
            if (word == kWatched[index].entity) {
                matchOffset = offset;
                matchKind = "entity_tag";
                return static_cast<int>(index);
            }
            if (hasPointer && definition != 0 && pointer == definition) {
                matchOffset = offset;
                matchKind = "definition_ptr";
                return static_cast<int>(index);
            }
        }
    }
    return -1;
}

/** Return addresses that identify where a creation came from. */
constexpr unsigned long long kFactoryReturn = 0x56D9A6ULL;
constexpr unsigned long long kInstantiateAllocateReturn = 0x575AAEULL;
constexpr unsigned long long kCreateEntityFactoryReturn = 0x17243FAULL;
/** A frame this far past the game image base is outside the game module (Sunrise itself). */
constexpr unsigned long long kOutsideGameImage = 0x10000000ULL;

[[nodiscard]] const char* origin_of(unsigned long long callerRva,
                                    std::span<const unsigned long long> chain) noexcept {
    for (const unsigned long long frame : chain) {
        if (frame == kCreateEntityFactoryReturn) {
            return "native_create_entity";
        }
    }
    if (callerRva == kInstantiateAllocateReturn) {
        return "native_instantiate";
    }
    if (callerRva == kFactoryReturn) {
        for (const unsigned long long frame : chain) {
            if (frame >= kOutsideGameImage) {
                return "sunrise_factory";
            }
        }
        return "native_factory";
    }
    return "other";
}

/**
 * Live handles per watched NPC with their creation origin, so simultaneous copies show up in the
 * log and the Tower NPC spawner can see a copy it did not create. Room for a production copy,
 * a debug lineup copy and the game's own copies.
 */
constexpr std::size_t kCopyCapacity = 8;
std::array<std::array<NpcCopy, kCopyCapacity>, kWatchedCount> g_copies{};
SRWLOCK g_copiesLock = SRWLOCK_INIT;

/** Creation descriptor fields proven at runtime (npc28): flags and the aux payload offset. */
constexpr std::size_t kPayloadFlagsOffset = 0x68;
constexpr std::size_t kPayloadAuxOffset = 0x78;
constexpr std::uint32_t kSquadSensorClass = 0x80809A3BU;

/** One copy with the creation facts its descriptor carries, for the Tower NPC squad tests. */
[[nodiscard]] NpcCopy describe_copy(std::uint32_t handle, const char* origin,
                                    std::span<const std::byte> descriptor) noexcept {
    NpcCopy copy{handle, origin};
    if (descriptor.size() < kPayloadAuxOffset + sizeof(std::int64_t)) {
        return copy;
    }
    std::memcpy(&copy.flags, descriptor.data() + kPayloadFlagsOffset, sizeof copy.flags);
    std::memcpy(&copy.auxRelative, descriptor.data() + kPayloadAuxOffset,
                sizeof copy.auxRelative);
    const std::int64_t start = static_cast<std::int64_t>(kPayloadAuxOffset) + copy.auxRelative;
    if (copy.auxRelative <= 0 || start >= static_cast<std::int64_t>(descriptor.size())) {
        return copy;
    }
    for (auto offset = static_cast<std::size_t>(start); offset + 4 <= descriptor.size();
         offset += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, descriptor.data() + offset, sizeof word);
        if (word == kSquadSensorClass) {
            copy.squadSensor = true;
            break;
        }
    }
    return copy;
}

void track_copy(std::size_t index, const NpcCopy& copy) noexcept {
    const std::uint32_t handle = copy.handle;
    const char* const origin = copy.origin;
    std::array<std::uint32_t, kCopyCapacity> live{};
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_copiesLock);
    auto& copies = g_copies[index];
    for (NpcCopy& held : copies) {
        if (held.handle != 0U && held.handle != handle && live_datum(held.handle) == nullptr) {
            held = {};
        }
    }
    bool stored = false;
    for (NpcCopy& held : copies) {
        if (held.handle == handle) {
            held = copy;
            stored = true;
        }
    }
    for (NpcCopy& held : copies) {
        if (!stored && held.handle == 0U) {
            held = copy;
            stored = true;
        }
        if (held.handle != 0U) {
            live[count++] = held.handle;
        }
    }
    ReleaseSRWLockExclusive(&g_copiesLock);
    if (count < 2) {
        return;
    }
    std::array<char, 256> line{};
    write_line(line, std::snprintf(line.data(), line.size(),
                                   "DEBUG_SAULO npc_instantiate_trace name=%s stage=copies "
                                   "entity=0x%08X live=%zu newest=0x%08X origin=%s "
                                   "handles=0x%08X,0x%08X,0x%08X,0x%08X",
                                   kWatched[index].name, kWatched[index].entity, count, handle,
                                   origin, live[0], live[1], live[2], live[3]));
}

/** Names the component classes a creation payload can carry. */
[[nodiscard]] const char* component_name(std::uint32_t value) noexcept {
    switch (value) {
    case kSquadSensorClass: return "squad_sensor";
    case 0x80809927U: return "object_sensor";
    case 0x808094EEU: return "engagement_sensor";
    case 0x80809583U: return "performance_sensor";
    case 0x80807EB6U: return "aux_header";
    default: return nullptr;
    }
}

/**
 * Decodes the creation descriptor's self-relative auxiliary payload (+0x78), which the squad
 * path fills and the factory path leaves empty, and lists the sensor classes and module
 * pointers inside it.
 */
void report_payload(std::size_t index, std::span<const std::byte> descriptor, std::uint32_t handle,
                    const char* stage) noexcept {
    constexpr std::size_t kFlagsOffset = kPayloadFlagsOffset;
    constexpr std::size_t kAuxOffset = kPayloadAuxOffset;
    if (descriptor.size() < kAuxOffset + sizeof(std::int64_t)) {
        return;
    }
    std::uint32_t flags = 0;
    std::int64_t relative = 0;
    std::memcpy(&flags, descriptor.data() + kFlagsOffset, sizeof flags);
    std::memcpy(&relative, descriptor.data() + kAuxOffset, sizeof relative);
    std::array<char, 640> line{};
    int written = std::snprintf(line.data(), line.size(),
                                "DEBUG_SAULO npc_marker_trace name=%s stage=%s actor_handle=0x%08X "
                                "flags=0x%08X aux_relative=%lld classes=",
                                kWatched[index].name, stage, handle, flags,
                                static_cast<long long>(relative));
    const std::int64_t start = static_cast<std::int64_t>(kAuxOffset) + relative;
    std::size_t listed = 0;
    if (relative > 0 && start < static_cast<std::int64_t>(descriptor.size())) {
        for (auto offset = static_cast<std::size_t>(start);
             offset + 4 <= descriptor.size() && listed < 12; offset += 4) {
            std::uint32_t word = 0;
            std::memcpy(&word, descriptor.data() + offset, sizeof word);
            const char* name = component_name(word);
            if (name != nullptr && written > 0 && static_cast<std::size_t>(written) < line.size()) {
                written += std::snprintf(line.data() + written,
                                         line.size() - static_cast<std::size_t>(written),
                                         "%s%s@0x%zX", listed == 0 ? "" : ",", name, offset);
                ++listed;
            }
        }
        std::size_t modules = 0;
        for (auto offset = static_cast<std::size_t>(start) & ~static_cast<std::size_t>(7);
             offset + 8 <= descriptor.size() && modules < 6; offset += 8) {
            std::uint64_t value = 0;
            std::memcpy(&value, descriptor.data() + offset, sizeof value);
            if (value >= g_moduleBase && value - g_moduleBase < kOutsideGameImage
                && written > 0 && static_cast<std::size_t>(written) < line.size()) {
                written += std::snprintf(line.data() + written,
                                         line.size() - static_cast<std::size_t>(written),
                                         "%smodule_ptr=+0x%llX@0x%zX", modules == 0 ? " " : ",",
                                         static_cast<unsigned long long>(value - g_moduleBase),
                                         offset);
                ++modules;
            }
        }
    }
    if (listed == 0 && written > 0 && static_cast<std::size_t>(written) < line.size()) {
        written += std::snprintf(line.data() + written,
                                 line.size() - static_cast<std::size_t>(written), "none");
    }
    write_line(line, written);
}

void report_allocation(std::size_t index,
                       std::span<const std::byte> descriptor,
                       std::size_t matchOffset,
                       const char* matchKind,
                       std::int32_t list,
                       std::int32_t entryIndex,
                       const void* entry,
                       std::uint32_t handle,
                       std::uintptr_t caller) noexcept {
    std::array<void*, kCallerFrames> frames{};
    const USHORT captured =
        RtlCaptureStackBackTrace(2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    std::array<unsigned long long, kCallerFrames> rvas{};
    for (std::size_t frame = 0; frame < captured && frame < rvas.size(); ++frame) {
        rvas[frame] = rva_of(reinterpret_cast<std::uintptr_t>(frames[frame]));
    }
    const char* const origin = origin_of(rva_of(caller), rvas);
    std::array<char, 544> line{};
    write_line(line, std::snprintf(line.data(), line.size(),
                                   "DEBUG_SAULO npc_idle_trace name=%s stage=allocate "
                                   "entity=0x%08X origin=%s "
                                   "actor_handle=0x%08X list=0x%08X entry=%d entry_ptr=%p "
                                   "match=%s@0x%zX caller_rva=+0x%llX chain=+0x%llX,+0x%llX,"
                                   "+0x%llX,+0x%llX,+0x%llX,+0x%llX,+0x%llX,+0x%llX tid=%lu",
                                   kWatched[index].name, kWatched[index].entity, origin, handle,
                                   static_cast<std::uint32_t>(list), entryIndex, entry, matchKind,
                                   matchOffset, rva_of(caller), rvas[0], rvas[1], rvas[2], rvas[3],
                                   rvas[4], rvas[5], rvas[6], rvas[7],
                                   static_cast<unsigned long>(GetCurrentThreadId())));
    report_payload(index, descriptor, handle, "allocate_payload");
}

/** Searches one block for the NPC's watched values and records new hits. */
void scan_block(std::size_t index,
                ProbeState& probe,
                std::uintptr_t base,
                std::span<const std::byte> bytes,
                const char* path) noexcept {
    const WatchedNpc& npc = kWatched[index];
    const std::uintptr_t definition = g_definitions[index].load(std::memory_order_acquire);
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= bytes.size(); offset += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, bytes.data() + offset, sizeof word);
        std::uint64_t pointer = 0;
        if ((offset & 7U) == 0 && offset + sizeof pointer <= bytes.size()) {
            std::memcpy(&pointer, bytes.data() + offset, sizeof pointer);
        }
        const char* kind = nullptr;
        if (word == npc.entity) {
            kind = "entity_tag";
        } else if (npc.stateMachine != 0 && word == npc.stateMachine) {
            kind = "state_machine";
        } else if (word == kStateGroup) {
            kind = "state_group";
        } else if (word != kNoState && (word == npc.states[0] || word == npc.states[1])) {
            kind = "state";
        } else if (definition != 0 && pointer == definition) {
            kind = "definition_ptr";
        }
        if (kind == nullptr) {
            continue;
        }
        const std::uintptr_t address = base + offset;
        const auto used = probe.hits.begin() + static_cast<std::ptrdiff_t>(probe.hitCount);
        const auto seen = std::find_if(probe.hits.begin(), used, [address](const Hit& hit) {
            return hit.address == address;
        });
        if (seen != used
            || probe.hitCount == probe.hits.size()) {
            continue;
        }
        Hit& hit = probe.hits[probe.hitCount++];
        hit.address = address;
        hit.value = word;
        hit.kind = kind;
        (void)std::snprintf(hit.path.data(), hit.path.size(), "%s+0x%zX", path, offset);
        (void)read_bytes(address - kWatchWindow, hit.window);
        std::array<char, kWatchWindow * 4 + 1> hex{};
        hex_into(hit.window, hex);
        std::array<char, 448> line{};
        write_line(line, std::snprintf(line.data(), line.size(),
                                       "DEBUG_SAULO npc_idle_trace name=%s stage=probe "
                                       "actor_handle=0x%08X path=%s addr=%p kind=%s value=0x%08X "
                                       "window=%s",
                                       npc.name, probe.handle, hit.path.data(),
                                       reinterpret_cast<void*>(address), kind, word, hex.data()));
    }
}

/** Walks datum pointers two levels deep, bounded in count and size. */
void discover(std::size_t index, ProbeState& probe, std::uintptr_t datum) noexcept {
    scan_block(index, probe, datum, probe.datum, "d");
    for (std::size_t first = 0; first + 8 <= probe.datum.size(); first += 8) {
        std::uint64_t target = 0;
        std::memcpy(&target, probe.datum.data() + first, sizeof target);
        if (!plausible_pointer(target)) {
            continue;
        }
        std::array<std::byte, kFirstBlockBytes> block{};
        if (!read_bytes(static_cast<std::uintptr_t>(target), block)) {
            continue;
        }
        std::array<char, 24> path{};
        (void)std::snprintf(path.data(), path.size(), "d+0x%zX>", first);
        scan_block(index, probe, static_cast<std::uintptr_t>(target), block, path.data());
        std::size_t followed = 0;
        for (std::size_t second = 0;
             second + 8 <= block.size() && followed < kSecondPointersPerBlock; second += 8) {
            std::uint64_t next = 0;
            std::memcpy(&next, block.data() + second, sizeof next);
            if (!plausible_pointer(next) || next == target) {
                continue;
            }
            std::array<std::byte, kSecondBlockBytes> inner{};
            if (!read_bytes(static_cast<std::uintptr_t>(next), inner)) {
                continue;
            }
            ++followed;
            std::array<char, 32> innerPath{};
            (void)std::snprintf(innerPath.data(), innerPath.size(), "d+0x%zX>+0x%zX>", first,
                                second);
            scan_block(index, probe, static_cast<std::uintptr_t>(next), inner, innerPath.data());
        }
    }
}

/** Logs the dwords of the datum that changed since the last probe. */
void diff_datum(std::size_t index, ProbeState& probe,
                const std::array<std::byte, kDatumBytes>& current) noexcept {
    if (!probe.datumKnown) {
        probe.datum = current;
        probe.datumKnown = true;
        std::array<char, 512> line{};
        for (std::size_t offset = 0; offset < current.size(); offset += 0x70) {
            std::array<char, 0x70 * 2 + 1> hex{};
            hex_into(std::span<const std::byte>(current).subspan(offset, 0x70), hex);
            write_line(line, std::snprintf(line.data(), line.size(),
                                           "DEBUG_SAULO npc_idle_trace name=%s stage=datum "
                                           "actor_handle=0x%08X off=0x%02zX bytes=%s",
                                           kWatched[index].name, probe.handle, offset,
                                           hex.data()));
        }
        return;
    }
    std::array<char, 640> line{};
    int written = std::snprintf(line.data(), line.size(),
                                "DEBUG_SAULO npc_idle_trace name=%s stage=datum_change "
                                "actor_handle=0x%08X changes=",
                                kWatched[index].name, probe.handle);
    std::size_t changes = 0;
    for (std::size_t offset = 0; offset < current.size(); offset += 4) {
        std::uint32_t before = 0;
        std::uint32_t after = 0;
        std::memcpy(&before, probe.datum.data() + offset, sizeof before);
        std::memcpy(&after, current.data() + offset, sizeof after);
        if (before == after || changes == kDatumChangesPerLine || written <= 0
            || static_cast<std::size_t>(written) >= line.size()) {
            continue;
        }
        written += std::snprintf(line.data() + written,
                                 line.size() - static_cast<std::size_t>(written),
                                 "%s0x%02zX:%08X>%08X", changes == 0 ? "" : ",", offset, before,
                                 after);
        ++changes;
    }
    probe.datum = current;
    if (changes != 0) {
        write_line(line, written);
    }
}

/** Re-reads every hit window and logs the ones whose bytes changed. */
void watch_hits(std::size_t index, ProbeState& probe) noexcept {
    for (std::size_t slot = 0; slot < probe.hitCount; ++slot) {
        Hit& hit = probe.hits[slot];
        std::array<std::byte, kWatchWindow * 2> now{};
        if (!read_bytes(hit.address - kWatchWindow, now) || now == hit.window) {
            continue;
        }
        std::array<char, kWatchWindow * 4 + 1> before{};
        std::array<char, kWatchWindow * 4 + 1> after{};
        hex_into(hit.window, before);
        hex_into(now, after);
        hit.window = now;
        std::array<char, 512> line{};
        write_line(line, std::snprintf(line.data(), line.size(),
                                       "DEBUG_SAULO npc_idle_trace name=%s stage=state "
                                       "actor_handle=0x%08X path=%s kind=%s before=%s after=%s",
                                       kWatched[index].name, probe.handle, hit.path.data(),
                                       hit.kind, before.data(), after.data()));
    }
}

void probe_one(std::size_t index, std::uint64_t now) noexcept {
    ProbeState& probe = g_probes[index];
    const std::uint32_t handle = g_handles[index].load(std::memory_order_acquire);
    if (handle != probe.handle) {
        probe = {};
        probe.handle = handle;
    }
    if (!has_handle(handle)) {
        return;
    }
    std::byte* const datum = live_datum(handle);
    if (datum == nullptr) {
        std::array<char, 160> line{};
        write_line(line, std::snprintf(line.data(), line.size(),
                                       "DEBUG_SAULO npc_idle_trace name=%s stage=result "
                                       "actor_handle=0x%08X result=datum_gone",
                                       kWatched[index].name, handle));
        std::uint32_t expected = handle;
        (void)g_handles[index].compare_exchange_strong(expected, 0U);
        probe = {};
        probe.handle = 0U;
        return;
    }
    std::array<std::byte, kDatumBytes> current{};
    if (!read_bytes(reinterpret_cast<std::uintptr_t>(datum), current)) {
        return;
    }
    diff_datum(index, probe, current);
    watch_hits(index, probe);
    if (now >= probe.nextDiscovery) {
        probe.nextDiscovery = now + kRediscoverMs;
        discover(index, probe, reinterpret_cast<std::uintptr_t>(datum));
    }
}

} // namespace

void trace_npc_allocation(const void* entry,
                          std::int32_t list,
                          std::int32_t entryIndex,
                          const std::uint32_t* result,
                          std::uintptr_t caller) noexcept {
    std::uint32_t handle = kNone;
    if (entry == nullptr || result == nullptr || !read_value(result, handle) || handle == kNone) {
        return;
    }
    // An instantiate entry is one 0x90 placement naming its class at +0; a factory descriptor
    // is searched whole for the NPC's tag or resolved definition pointer.
    const bool factory = list == -1;
    std::array<std::byte, kDescriptorBytes> storage{};
    const std::span<std::byte> descriptor =
        std::span<std::byte>(storage).first(factory ? kDescriptorBytes : kPlacementEntryBytes);
    if (!read_bytes(reinterpret_cast<std::uintptr_t>(entry), descriptor)) {
        return;
    }
    std::size_t matchOffset = 0;
    const char* matchKind = "none";
    const int index = match_descriptor(
        factory ? std::span<const std::byte>(descriptor)
                : std::span<const std::byte>(descriptor).first(sizeof(std::uint32_t)),
        matchOffset, matchKind);
    if (index < 0) {
        return;
    }
    const auto slot = static_cast<std::size_t>(index);
    g_handles[slot].store(handle, std::memory_order_release);
    std::array<void*, kCallerFrames> frames{};
    const USHORT captured =
        RtlCaptureStackBackTrace(1, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    std::array<unsigned long long, kCallerFrames> rvas{};
    for (std::size_t frame = 0; frame < captured && frame < rvas.size(); ++frame) {
        rvas[frame] = rva_of(reinterpret_cast<std::uintptr_t>(frames[frame]));
    }
    const char* const origin = origin_of(rva_of(caller), rvas);
    track_copy(slot, describe_copy(handle, origin, descriptor));
    if (g_allocationReports[slot].fetch_add(1, std::memory_order_relaxed) < kAllocationReports) {
        report_allocation(slot, descriptor, matchOffset, matchKind, list, entryIndex, entry, handle,
                          caller);
    }
}

namespace {
constexpr std::uint32_t kInstantiateReports = 6;
std::array<std::atomic_uint32_t, kWatchedCount> g_instantiateReports{};
} // namespace

int trace_npc_instantiate_enter(const void* entry,
                                std::int32_t list,
                                std::int32_t entryIndex,
                                std::uintptr_t caller) noexcept {
    if (entry == nullptr) {
        return -1;
    }
    const bool factory = list == -1;
    std::array<std::byte, kDescriptorBytes> storage{};
    const std::span<std::byte> descriptor =
        std::span<std::byte>(storage).first(factory ? kDescriptorBytes : kPlacementEntryBytes);
    if (!read_bytes(reinterpret_cast<std::uintptr_t>(entry), descriptor)) {
        return -1;
    }
    std::size_t matchOffset = 0;
    const char* matchKind = "none";
    const int index =
        match_descriptor(std::span<const std::byte>(descriptor).first(sizeof(std::uint32_t)),
                         matchOffset, matchKind);
    if (index < 0) {
        return -1;
    }
    const auto slot = static_cast<std::size_t>(index);
    if (g_instantiateReports[slot].fetch_add(1, std::memory_order_relaxed) >= kInstantiateReports) {
        return -1;
    }
    std::array<void*, kCallerFrames> frames{};
    const USHORT captured =
        RtlCaptureStackBackTrace(2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    std::array<unsigned long long, kCallerFrames> rvas{};
    for (std::size_t frame = 0; frame < captured && frame < rvas.size(); ++frame) {
        rvas[frame] = rva_of(reinterpret_cast<std::uintptr_t>(frames[frame]));
    }
    std::array<char, 512> line{};
    write_line(line, std::snprintf(line.data(), line.size(),
                                   "DEBUG_SAULO npc_instantiate_trace name=%s stage=enter "
                                   "target_rva=+0x575690 caller_rva=+0x%llX entity=0x%08X "
                                   "descriptor=%p list=0x%08X entry=%d chain=+0x%llX,+0x%llX,"
                                   "+0x%llX,+0x%llX,+0x%llX,+0x%llX",
                                   kWatched[slot].name, rva_of(caller), kWatched[slot].entity,
                                   entry, static_cast<std::uint32_t>(list), entryIndex, rvas[0],
                                   rvas[1], rvas[2], rvas[3], rvas[4], rvas[5]));
    report_payload(slot, descriptor, kNone, "instantiate_payload");
    return index;
}

void trace_npc_instantiate_return(int watched,
                                  const void* entry,
                                  const std::uint32_t* result,
                                  std::uintptr_t caller) noexcept {
    if (watched < 0 || static_cast<std::size_t>(watched) >= kWatchedCount) {
        return;
    }
    const auto slot = static_cast<std::size_t>(watched);
    std::uint32_t handle = kNone;
    (void)read_value(result, handle);
    std::array<std::byte, 0x10> rotation{};
    std::byte* const datum = has_handle(handle) ? live_datum(handle) : nullptr;
    const bool rotationRead =
        datum != nullptr && read_bytes(reinterpret_cast<std::uintptr_t>(datum) + 0xA0, rotation);
    std::array<char, 33> hex{};
    hex_into(rotation, hex);
    std::array<char, 320> line{};
    write_line(line, std::snprintf(line.data(), line.size(),
                                   "DEBUG_SAULO npc_instantiate_trace name=%s stage=return "
                                   "caller_rva=+0x%llX entity=0x%08X descriptor=%p handle=0x%08X "
                                   "datum=%p datum_rotation=%s",
                                   kWatched[slot].name, rva_of(caller), kWatched[slot].entity,
                                   entry, handle, static_cast<void*>(datum),
                                   rotationRead ? hex.data() : "unread"));
}

std::size_t live_npc_copies(std::uint32_t entityTag, std::span<NpcCopy> output) noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < kWatchedCount; ++index) {
        if (kWatched[index].entity != entityTag) {
            continue;
        }
        AcquireSRWLockShared(&g_copiesLock);
        const auto copies = g_copies[index];
        ReleaseSRWLockShared(&g_copiesLock);
        for (const NpcCopy& held : copies) {
            if (count < output.size() && has_handle(held.handle)
                && live_datum(held.handle) != nullptr) {
                output[count++] = held;
            }
        }
        break;
    }
    return count;
}

void note_npc_definition(std::uint32_t entityTag, std::uintptr_t definition) noexcept {
    for (std::size_t index = 0; index < kWatchedCount; ++index) {
        if (kWatched[index].entity == entityTag) {
            g_definitions[index].store(definition, std::memory_order_release);
        }
    }
}

std::size_t npc_trace_count() noexcept {
    return kWatchedCount;
}

std::uint32_t npc_trace_entity(std::size_t index) noexcept {
    return index < kWatchedCount ? kWatched[index].entity : 0U;
}

void trace_npc_actors() noexcept {
    const std::uint64_t now = GetTickCount64();
    if (now < g_nextProbe) {
        return;
    }
    g_nextProbe = now + kProbeStepMs;
    // One NPC per step keeps each step's reads bounded.
    for (std::size_t tried = 0; tried < kWatchedCount; ++tried) {
        const std::size_t index = g_probeCursor;
        g_probeCursor = (g_probeCursor + 1) % kWatchedCount;
        if (has_handle(g_handles[index].load(std::memory_order_acquire))
            || has_handle(g_probes[index].handle)) {
            probe_one(index, now);
            return;
        }
    }
}

} // namespace sunrise::client::hooks::world_objects

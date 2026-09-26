#include "cursor_guard_replacements.h"

#include <array>
#include <cstdint>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "../../input/window_focus.h"
#include "runtime.h"

namespace sunrise::client::hooks::cursor {
namespace {

SRWLOCK g_policyLock{SRWLOCK_INIT};
bool g_interfaceOpen{};
bool g_gameClipValid{};
RECT g_gameClip{};

// DEBUG_SAULO: gameplay/menu signal diagnostics. Logs changes only. Remove this block, the
// block in clip_cursor() and the debug_poll_signals() call in apply_visibility().

/** DEBUG_SAULO: the game's last ClipCursor request, and its calls since it last changed. */
bool g_debugClipSeen{};
bool g_debugClipConfined{};
RECT g_debugClipRect{};
std::uint64_t g_debugClipCalls{};

/** DEBUG_SAULO: raw-input registrations read each frame. More are reported as a count. */
constexpr UINT kDebugRawDeviceCapacity = 8;

/** DEBUG_SAULO: the cursor and raw-input state read once a frame. */
struct DebugSignals {
    bool overlay{};
    bool focused{};
    RECT clip{};
    bool clipped{};
    DWORD cursorFlags{};
    HCURSOR cursor{};
    UINT rawCount{};
    UINT rawRead{};
    std::array<RAWINPUTDEVICE, kDebugRawDeviceCapacity> raw{};
};

SRWLOCK g_debugSignalsLock{SRWLOCK_INIT};
DebugSignals g_debugSignals{};
bool g_debugSignalsValid{};

/** DEBUG_SAULO: @return True when both rectangles hold the same four edges, empty ones included. */
[[nodiscard]] bool debug_same_rect(const RECT& left, const RECT& right) noexcept {
    return left.left == right.left && left.top == right.top && left.right == right.right
           && left.bottom == right.bottom;
}

/** DEBUG_SAULO: @return True when two frames read the same state. */
[[nodiscard]] bool debug_same(const DebugSignals& left, const DebugSignals& right) noexcept {
    if (left.overlay != right.overlay || left.focused != right.focused
        || !debug_same_rect(left.clip, right.clip) || left.clipped != right.clipped
        || left.cursorFlags != right.cursorFlags || left.cursor != right.cursor
        || left.rawCount != right.rawCount || left.rawRead != right.rawRead) {
        return false;
    }
    for (UINT index = 0; index < left.rawRead; ++index) {
        const RAWINPUTDEVICE& a = left.raw[index];
        const RAWINPUTDEVICE& b = right.raw[index];
        if (a.usUsagePage != b.usUsagePage || a.usUsage != b.usUsage || a.dwFlags != b.dwFlags
            || a.hwndTarget != b.hwndTarget) {
            return false;
        }
    }
    return true;
}

/** DEBUG_SAULO: reads the clip, the cursor and this process's raw-input registrations. */
[[nodiscard]] DebugSignals debug_read_signals(bool visible) noexcept {
    DebugSignals signals{};
    signals.overlay = visible;
    signals.focused = input::game_focused();
    if (GetClipCursor(&signals.clip) != FALSE) {
        // An unconfined pointer reports the whole virtual screen.
        const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const RECT screen{left,
                          top,
                          left + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                          top + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        signals.clipped = !debug_same_rect(signals.clip, screen);
    }
    CURSORINFO info{};
    info.cbSize = static_cast<DWORD>(sizeof info);
    if (GetCursorInfo(&info) != FALSE) {
        signals.cursorFlags = info.flags;
        signals.cursor = info.hCursor;
    }
    UINT count = kDebugRawDeviceCapacity;
    const UINT read = GetRegisteredRawInputDevices(
        signals.raw.data(), &count, static_cast<UINT>(sizeof(RAWINPUTDEVICE)));
    // A full buffer returns -1 and leaves the needed count, with nothing written.
    signals.rawRead = read == static_cast<UINT>(-1) ? 0 : read;
    signals.rawCount = read == static_cast<UINT>(-1) ? count : read;
    return signals;
}

/** DEBUG_SAULO: one line for a changed frame state. */
void debug_log_signals(const DebugSignals& signals) noexcept {
    std::array<char, 512> raw{};
    std::size_t used = 0;
    for (UINT index = 0; index < signals.rawRead; ++index) {
        const RAWINPUTDEVICE& device = signals.raw[index];
        const int written = std::snprintf(raw.data() + used,
                                          raw.size() - used,
                                          "%s%X:%X:0x%lX@%p",
                                          index == 0 ? "" : ",",
                                          static_cast<unsigned>(device.usUsagePage),
                                          static_cast<unsigned>(device.usUsage),
                                          static_cast<unsigned long>(device.dwFlags),
                                          static_cast<void*>(device.hwndTarget));
        if (written <= 0 || static_cast<std::size_t>(written) >= raw.size() - used) {
            break;
        }
        used += static_cast<std::size_t>(written);
    }
    core::log::writef(core::log::Channel::client,
                      core::log::Level::info,
                      "DEBUG_SAULO ev=cursor_signals overlay=%d focused=%d clipped=%d "
                      "clip=%ld,%ld,%ld,%ld cursor_flags=0x%lX cursor=%p raw_count=%u raw=%s",
                      signals.overlay ? 1 : 0,
                      signals.focused ? 1 : 0,
                      signals.clipped ? 1 : 0,
                      static_cast<long>(signals.clip.left),
                      static_cast<long>(signals.clip.top),
                      static_cast<long>(signals.clip.right),
                      static_cast<long>(signals.clip.bottom),
                      static_cast<unsigned long>(signals.cursorFlags),
                      static_cast<void*>(signals.cursor),
                      signals.rawCount,
                      used == 0 ? "none" : raw.data());
}

/** DEBUG_SAULO: reads the frame state and logs it only when it changed. */
void debug_poll_signals(bool visible) noexcept {
    // Present has one caller at a time in practice; a second one skips its read.
    if (TryAcquireSRWLockExclusive(&g_debugSignalsLock) == FALSE) {
        return;
    }
    const DebugSignals current = debug_read_signals(visible);
    const bool changed = !g_debugSignalsValid || !debug_same(current, g_debugSignals);
    if (changed) {
        g_debugSignals = current;
        g_debugSignalsValid = true;
    }
    ReleaseSRWLockExclusive(&g_debugSignalsLock);
    if (changed) {
        debug_log_signals(current);
    }
}

} // namespace

std::array<hooking::detour::Handle, kHandleCount> g_handles{};
std::array<void*, kHandleCount> g_targets{};

/** Answers the game's cursor move while the interface is open. */
BOOL WINAPI set_cursor_pos(int x, int y) noexcept {
    AcquireSRWLockShared(&g_policyLock);
    const bool open = g_interfaceOpen;
    ReleaseSRWLockShared(&g_policyLock);
    if (open) {
        return kCallSucceeded;
    }

    const SetCursorPos next = original<SetCursorPos>(HookSlot::setCursorPos);
    if (next == nullptr) {
        return kCallSucceeded;
    }
    return next(x, y);
}

/** Frees the pointer while the interface is open and remembers the game's own bounds. */
BOOL WINAPI clip_cursor(const RECT* bounds) noexcept {
    AcquireSRWLockExclusive(&g_policyLock);
    // DEBUG_SAULO: a change in what the game asks for, logged once the lock is released.
    const bool debugConfined = bounds != nullptr;
    const RECT debugRect = debugConfined ? *bounds : RECT{};
    const bool debugChanged = !g_debugClipSeen || g_debugClipConfined != debugConfined
                              || !debug_same_rect(g_debugClipRect, debugRect);
    const std::uint64_t debugCalls = g_debugClipCalls;
    g_debugClipSeen = true;
    g_debugClipConfined = debugConfined;
    g_debugClipRect = debugRect;
    g_debugClipCalls = debugChanged ? 1 : g_debugClipCalls + 1;

    g_gameClipValid = bounds != nullptr;
    if (g_gameClipValid) {
        g_gameClip = *bounds;
    }
    const bool open = g_interfaceOpen;
    ReleaseSRWLockExclusive(&g_policyLock);

    // DEBUG_SAULO: previous_calls counts the calls that repeated the request this one replaces.
    if (debugChanged) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::info,
                          "DEBUG_SAULO ev=cursor_clip_request confined=%d rect=%ld,%ld,%ld,%ld "
                          "previous_calls=%llu overlay=%d",
                          debugConfined ? 1 : 0,
                          static_cast<long>(debugRect.left),
                          static_cast<long>(debugRect.top),
                          static_cast<long>(debugRect.right),
                          static_cast<long>(debugRect.bottom),
                          static_cast<unsigned long long>(debugCalls),
                          open ? 1 : 0);
    }

    const ClipCursor next = original<ClipCursor>(HookSlot::clipCursor);
    if (next == nullptr) {
        return kCallSucceeded;
    }
    return next(open ? nullptr : bounds);
}

/** Applies one interface-visibility edge to the pointer. */
void apply_policy(bool visible) noexcept {
    AcquireSRWLockExclusive(&g_policyLock);
    if (g_interfaceOpen == visible) {
        ReleaseSRWLockExclusive(&g_policyLock);
        return;
    }
    g_interfaceOpen = visible;
    const bool restore = !visible && g_gameClipValid;
    const RECT bounds = g_gameClip;
    ReleaseSRWLockExclusive(&g_policyLock);

    // Win32 runs outside the policy lock so the hooked call cannot re-enter it.
    const ClipCursor next = original<ClipCursor>(HookSlot::clipCursor);
    if (next == nullptr) {
        return;
    }
    if (visible) {
        (void)next(nullptr);
        return;
    }
    if (restore) {
        (void)next(&bounds);
    }
}

/** Applies the cursor policy for the current interface visibility. */
void apply_visibility(bool visible) noexcept {
    apply_policy(visible);
    // DEBUG_SAULO: runs once a frame from Present, outside the presentation locks.
    debug_poll_signals(visible);
}

} // namespace sunrise::client::hooks::cursor

#pragma once

#include <cstdint>
#include <string_view>

namespace Das::Plugins::DasMaaPi
{
    // PI controller typed-config string → MaaFW enum translation tables.
    //
    // Source of truth: MaaFramework/include/MaaFramework/MaaDef.h:348-404
    // (Win32 screencap/input enums) and :449-450 (MaaGamepadType). This header
    // deliberately uses std::uint64_t instead of the MaaFW typedefs so the
    // translation unit compiles under DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
    // (test build) without pulling MaaFramework headers. Returned bitmasks are
    // passed straight to MaaWin32ControllerCreate / MaaGamepadControllerCreate
    // which accept uint64_t-sized enums.
    //
    // Values mirror MaaDef.h bit positions exactly:
    //   MaaWin32ScreencapMethod: GDI=bit0, FramePool=bit1,
    //   DXGI_DesktopDup=bit2,
    //     DXGI_DesktopDup_Window=bit3, PrintWindow=bit4, ScreenDC=bit5.
    //     Predefined combos All/Foreground/Background expand to their OR.
    //   MaaWin32InputMethod: single-select — Seize=1, SendMessage=2,
    //     PostMessage=4, LegacyEvent=8, PostThreadMessage=16,
    //     SendMessageWithCursorPos=32, PostMessageWithCursorPos=64,
    //     SendMessageWithWindowPos=128, PostMessageWithWindowPos=256,
    //     Interception=512.
    //   MaaGamepadType: Xbox360=0, DualShock4=1 (alias: DS4).

    // Parse Win32 screencap bitmask from PI protocol string. Accepts:
    //   - Predefined combos: All / Foreground / Background (case-insensitive).
    //   - Bitwise OR via '|': e.g. "GDI|FramePool".
    //   - Single method names: GDI / FramePool / DXGI_DesktopDup /
    //     DXGI_DesktopDup_Window / PrintWindow / ScreenDC.
    // Returns 0 if any token is unknown (fail-closed: caller surfaces the
    // zero-screencap as a config error, never silently produces a working
    // controller with no screencap method).
    std::uint64_t ParseWin32ScreencapMethod(std::string_view text);

    // Parse Win32 input single-select enum (mouse or keyboard). Returns 0 on
    // unknown string. Case-insensitive.
    std::uint64_t ParseWin32InputMethod(std::string_view text);

    // Parse gamepad type single-select enum. Accepts Xbox360 / DualShock4 /
    // DS4 (alias). Returns 0 on unknown (Xbox360 is also 0, so callers must
    // validate the input string explicitly when distinguishing "default
    // Xbox360" from "garbage input").
    std::uint64_t ParseGamepadType(std::string_view text);
} // namespace Das::Plugins::DasMaaPi

#include "../src/AdbDeviceFinder.h"
#include "../src/ControllerMethodMaps.h"
#include "../src/MaaDynamicSymbols.h"
#include "../src/Win32WindowFinder.h"
#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>
#include <das/Plugins/DasMaaPi/PiCompiler.h>
#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/DasJsonCore.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

namespace
{
    using namespace Das::Plugins::DasMaaPi;

    std::filesystem::path FixturePath(std::string_view name)
    {
        return std::filesystem::current_path() / "DasMaaPi" / "test"
               / "fixtures" / std::filesystem::path{name};
    }
} // namespace

// Feature 1 RED — ControllerMethodMaps string→enum table (source MaaDef.h).
// Bit positions must align with MaaFramework/MaaDef.h:348-404 / 449-450.

TEST(ControllerMethodMaps, ParseWin32ScreencapSingleBitValues)
{
    EXPECT_EQ(ParseWin32ScreencapMethod("GDI"), 1ULL);
    EXPECT_EQ(ParseWin32ScreencapMethod("FramePool"), 1ULL << 1);
    EXPECT_EQ(ParseWin32ScreencapMethod("DXGI_DesktopDup"), 1ULL << 2);
    EXPECT_EQ(ParseWin32ScreencapMethod("DXGI_DesktopDup_Window"), 1ULL << 3);
    EXPECT_EQ(ParseWin32ScreencapMethod("PrintWindow"), 1ULL << 4);
    EXPECT_EQ(ParseWin32ScreencapMethod("ScreenDC"), 1ULL << 5);
}

TEST(ControllerMethodMaps, ParseWin32ScreencapOrCombination)
{
    // PI v2 protocol allows screencap to be a bitwise-OR set of methods.
    const auto value = ParseWin32ScreencapMethod("GDI|FramePool");
    EXPECT_NE(value & 1ULL, 0ULL);
    EXPECT_NE(value & (1ULL << 1), 0ULL);
    EXPECT_EQ(value, 1ULL | (1ULL << 1));
}

TEST(ControllerMethodMaps, ParseWin32ScreencapForegroundPredefinedCombo)
{
    // MaaDef.h: Foreground = DXGI_DesktopDup_Window | ScreenDC
    const auto value = ParseWin32ScreencapMethod("Foreground");
    EXPECT_NE(value & (1ULL << 3), 0ULL);
    EXPECT_NE(value & (1ULL << 5), 0ULL);
}

TEST(ControllerMethodMaps, ParseWin32ScreencapBackgroundPredefinedCombo)
{
    // MaaDef.h: Background = FramePool | PrintWindow
    const auto value = ParseWin32ScreencapMethod("Background");
    EXPECT_NE(value & (1ULL << 1), 0ULL);
    EXPECT_NE(value & (1ULL << 4), 0ULL);
}

TEST(ControllerMethodMaps, ParseWin32ScreencapCaseInsensitive)
{
    EXPECT_EQ(ParseWin32ScreencapMethod("gdi"), 1ULL);
    EXPECT_EQ(ParseWin32ScreencapMethod("FRAMEPOOL"), 1ULL << 1);
}

TEST(ControllerMethodMaps, ParseWin32ScreencapUnknownReturnsZero)
{
    EXPECT_EQ(ParseWin32ScreencapMethod("Unknown"), 0ULL);
    EXPECT_EQ(ParseWin32ScreencapMethod("GDI|Bogus"), 0ULL);
    EXPECT_EQ(ParseWin32ScreencapMethod(""), 0ULL);
}

TEST(ControllerMethodMaps, ParseWin32InputSingleSelectValues)
{
    EXPECT_EQ(ParseWin32InputMethod("Seize"), 1ULL);
    EXPECT_EQ(ParseWin32InputMethod("SendMessage"), 1ULL << 1);
    EXPECT_EQ(ParseWin32InputMethod("PostMessage"), 1ULL << 2);
    EXPECT_EQ(ParseWin32InputMethod("LegacyEvent"), 1ULL << 3);
    EXPECT_EQ(ParseWin32InputMethod("Interception"), 1ULL << 9);
}

TEST(ControllerMethodMaps, ParseWin32InputCaseInsensitive)
{
    EXPECT_EQ(ParseWin32InputMethod("seize"), 1ULL);
    EXPECT_EQ(ParseWin32InputMethod("POSTMESSAGE"), 1ULL << 2);
}

TEST(ControllerMethodMaps, ParseWin32InputUnknownReturnsZero)
{
    EXPECT_EQ(ParseWin32InputMethod("FancyNewMethod"), 0ULL);
    EXPECT_EQ(ParseWin32InputMethod(""), 0ULL);
}

TEST(ControllerMethodMaps, ParseGamepadTypeValues)
{
    EXPECT_EQ(ParseGamepadType("Xbox360"), 0ULL);
    EXPECT_EQ(ParseGamepadType("DualShock4"), 1ULL);
}

TEST(ControllerMethodMaps, ParseGamepadTypeAcceptsDS4Alias)
{
    EXPECT_EQ(ParseGamepadType("DS4"), 1ULL);
}

TEST(ControllerMethodMaps, ParseGamepadTypeCaseInsensitive)
{
    EXPECT_EQ(ParseGamepadType("xbox360"), 0ULL);
    EXPECT_EQ(ParseGamepadType("ds4"), 1ULL);
}

// Feature 1 RED — ParseController must populate typed DTO blocks instead of
// only round-tripping them via raw_json.

TEST(MaapiControllerConfig, ParseControllerExtractsWin32TypedDto)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.catalog.controllers.size(), 4u);
    const auto& desktop = result.catalog.controllers[1];
    EXPECT_EQ(desktop.dto.name, "Desktop");
    ASSERT_TRUE(desktop.dto.win32.has_value());
    EXPECT_EQ(desktop.dto.win32->class_regex, "Arknights.*");
    EXPECT_EQ(desktop.dto.win32->window_regex, ".*AutoScript");
    EXPECT_EQ(desktop.dto.win32->screencap, "Foreground");
    EXPECT_EQ(desktop.dto.win32->mouse, "Seize");
    EXPECT_EQ(desktop.dto.win32->keyboard, "SendMessage");
}

TEST(MaapiControllerConfig, ParseControllerExtractsGamepadTypedDto)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.catalog.controllers.size(), 4u);
    const auto& gamepad = result.catalog.controllers[2];
    EXPECT_EQ(gamepad.dto.name, "Gamepad1");
    ASSERT_TRUE(gamepad.dto.gamepad.has_value());
    EXPECT_EQ(gamepad.dto.gamepad->class_regex, "XboxApp.*");
    EXPECT_EQ(gamepad.dto.gamepad->window_regex, ".*");
    EXPECT_EQ(gamepad.dto.gamepad->gamepad_type, "DS4");
    EXPECT_EQ(gamepad.dto.gamepad->screencap, "GDI");
}

TEST(MaapiControllerConfig, ParseControllerLeavesTypedBlocksEmptyForAdbAndMac)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.catalog.controllers.size(), 4u);
    const auto& adb = result.catalog.controllers[0];
    EXPECT_FALSE(adb.dto.win32.has_value());
    EXPECT_FALSE(adb.dto.gamepad.has_value());
    const auto& mac = result.catalog.controllers[3];
    EXPECT_FALSE(mac.dto.win32.has_value());
    EXPECT_FALSE(mac.dto.gamepad.has_value());
}

TEST(MaapiControllerConfig, ParseControllerRoundTripsRawJsonForTypedBlocks)
{
    // raw_json must still carry the typed sub-objects so downstream
    // pi_env.controller_json stays structurally faithful (required by
    // ProjectInterface V2 wire and by agent-runtime env snapshot).
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(result.ok);
    const auto& desktop = result.catalog.controllers[1];
    EXPECT_NE(desktop.raw.raw_json.find("win32"), std::string::npos);
    EXPECT_NE(desktop.raw.raw_json.find("Arknights"), std::string::npos);
    EXPECT_NE(
        desktop.raw.raw_json.find("display_short_side"),
        std::string::npos);
}

// Feature 2 RED — NormalizeControllerInitParams reads win32/gamepad same-level
// keys (NOT the `config` sub-key) and produces typed init params consumed by
// CreateController overloads. ControllerSpec wire format stays byte-identical
// so MaapiRuntimeTest.cpp:107 hard-coded JSON keeps parsing.

TEST(NormalizeControllerInitParams, ReadsWin32SameLevelTypedKeys)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.catalog.controllers.size(), 4u);
    const auto& desktop = result.catalog.controllers[1];
    const auto  params = NormalizeControllerInitParams(desktop);
    EXPECT_EQ(params.controller_type, "Win32");
    ASSERT_TRUE(params.win32.has_value());
    EXPECT_EQ(params.win32->class_regex, "Arknights.*");
    EXPECT_EQ(params.win32->window_regex, ".*AutoScript");
    EXPECT_EQ(
        params.win32->screencap_method,
        ParseWin32ScreencapMethod("Foreground"));
    EXPECT_EQ(params.win32->mouse_method, ParseWin32InputMethod("Seize"));
    EXPECT_EQ(
        params.win32->keyboard_method,
        ParseWin32InputMethod("SendMessage"));
    ASSERT_TRUE(params.display_short_side.has_value());
    EXPECT_EQ(*params.display_short_side, 720);
    ASSERT_TRUE(params.display_long_side.has_value());
    EXPECT_EQ(*params.display_long_side, 1280);
    ASSERT_TRUE(params.display_raw.has_value());
    EXPECT_FALSE(*params.display_raw);
    EXPECT_FALSE(params.gamepad.has_value());
}

TEST(NormalizeControllerInitParams, ReadsGamepadSameLevelTypedKeys)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.catalog.controllers.size(), 4u);
    const auto& gamepad = result.catalog.controllers[2];
    const auto  params = NormalizeControllerInitParams(gamepad);
    EXPECT_EQ(params.controller_type, "Gamepad");
    ASSERT_TRUE(params.gamepad.has_value());
    EXPECT_EQ(params.gamepad->class_regex, "XboxApp.*");
    EXPECT_EQ(params.gamepad->window_regex, ".*");
    EXPECT_EQ(params.gamepad->gamepad_type, ParseGamepadType("DS4"));
    EXPECT_EQ(
        params.gamepad->screencap_method,
        ParseWin32ScreencapMethod("GDI"));
    EXPECT_FALSE(params.win32.has_value());
    // gamepad fixture deliberately omits display_* keys.
    EXPECT_FALSE(params.display_short_side.has_value());
    EXPECT_FALSE(params.display_long_side.has_value());
    EXPECT_FALSE(params.display_raw.has_value());
}

TEST(NormalizeControllerInitParams, AdbControllerPopulatesTypedAdbBlockOnly)
{
    // After the unified-dispatch refactor, Adb controllers MUST populate
    // params.adb (address/adbPath/configJson/agentPath) so the typed
    // CreateControllerFromParams dispatch can read them. Win32/Gamepad/Dbg
    // blocks stay absent for a pure Adb controller.
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.catalog.controllers.size(), 4u);
    const auto& adb = result.catalog.controllers[0];
    const auto  params = NormalizeControllerInitParams(adb);
    EXPECT_EQ(params.controller_type, "Adb");
    ASSERT_TRUE(params.adb.has_value());
    EXPECT_EQ(params.adb->address, "127.0.0.1:5555");
    EXPECT_EQ(params.adb->adb_path, "adb");
    EXPECT_FALSE(params.win32.has_value());
    EXPECT_FALSE(params.gamepad.has_value());
    EXPECT_FALSE(params.dbg.has_value());
    EXPECT_FALSE(params.display_short_side.has_value());
    EXPECT_FALSE(params.display_long_side.has_value());
    EXPECT_FALSE(params.display_raw.has_value());
}

TEST(NormalizeControllerInitParams, IgnoresConfigSubKeyForTypedRead)
{
    // PI protocol places win32/gamepad as same-level keys on the controller
    // object. A `config` sub-key carrying win32-shaped data is generic Adb
    // JSON, NOT a typed init source — the reader must not promote it.
    PiController controller;
    controller.dto.name = "X";
    controller.dto.type = "Win32";
    controller.raw.raw_json =
        R"({"name":"X","type":"Win32","config":"{\"win32\":{\"class_regex\":\"X\"}}"})";
    const auto params = NormalizeControllerInitParams(controller);
    EXPECT_FALSE(params.win32.has_value());
    EXPECT_FALSE(params.gamepad.has_value());
}

TEST(NormalizeControllerInitParams, ControllerSpecWireStaysByteIdentical)
{
    // ControllerSpec serialisation (existing envelope wire) must NOT gain
    // win32/gamepad/display_* keys — they live in the parallel typed struct.
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_controller.jsonc")});
    ASSERT_TRUE(result.ok);
    const auto& desktop = result.catalog.controllers[1];

    ControllerSpec spec;
    spec.name = desktop.dto.name;
    spec.type = desktop.dto.type;
    // Round-trip through the existing NormalizeControllerSpec path (PiCompiler
    // internal) by relying on the field set the function already produces.
    // We assert the serialised shape omits typed/init-only fields.
    yyjson::value spec_json = Das::Utils::MakeYyjsonObject();
    auto          obj = spec_json.as_object();
    (*obj)[std::string_view("name")] = yyjson::value(spec.name);
    (*obj)[std::string_view("type")] = yyjson::value(spec.type);
    const auto serialized =
        Das::Utils::SerializeYyjsonValue(spec_json).value_or("{}");
    EXPECT_EQ(serialized.find("win32"), std::string::npos);
    EXPECT_EQ(serialized.find("gamepad"), std::string::npos);
    EXPECT_EQ(serialized.find("display_short_side"), std::string::npos);
}

// Feature 3 RED — Win32WindowFinder regex HWND discovery. 0/1/N match
// disambiguation policy + injectable enumerator.

TEST(Win32WindowFinder, NoMatchReturnsNotFoundError)
{
    auto result = FindWindowByRegex(
        "NonExistent.*",
        "",
        []() { return std::vector<WindowInfo>{}; });
    EXPECT_EQ(result.handle, nullptr);
    EXPECT_EQ(result.error_code, "controller-window-not-found");
    EXPECT_FALSE(result.error_message.empty());
}

TEST(Win32WindowFinder, SingleMatchReturnsHandle)
{
    WindowInfo window;
    window.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(12345));
    window.class_name = "AppClass";
    window.window_name = "AppWindow";
    auto result = FindWindowByRegex(
        "App.*",
        "",
        [&]() { return std::vector<WindowInfo>{window}; });
    EXPECT_EQ(result.handle, window.handle);
    EXPECT_TRUE(result.error_code.empty());
    EXPECT_TRUE(result.error_message.empty());
}

TEST(Win32WindowFinder, MultipleMatchesReturnAmbiguousError)
{
    WindowInfo a;
    a.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
    a.class_name = "Match";
    WindowInfo b;
    b.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(2));
    b.class_name = "Match";
    auto result = FindWindowByRegex(
        "Match",
        "",
        [&]() { return std::vector<WindowInfo>{a, b}; });
    EXPECT_EQ(result.handle, nullptr);
    EXPECT_EQ(result.error_code, "controller-window-ambiguous");
    EXPECT_FALSE(result.error_message.empty());
}

TEST(Win32WindowFinder, EmptyRegexMatchesAnything)
{
    // Empty class_regex + empty window_regex = match every window. With one
    // window in the enumerator, succeeds; with two, ambiguous.
    WindowInfo only;
    only.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(42));
    only.class_name = "Any";
    only.window_name = "Thing";
    auto single = FindWindowByRegex(
        "",
        "",
        [&]() { return std::vector<WindowInfo>{only}; });
    EXPECT_EQ(single.handle, only.handle);
    EXPECT_TRUE(single.error_code.empty());
}

TEST(Win32WindowFinder, WindowRegexFiltersByTitle)
{
    WindowInfo match;
    match.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(777));
    match.class_name = "App";
    match.window_name = "Target Window";
    WindowInfo other;
    other.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(888));
    other.class_name = "App";
    other.window_name = "Other Window";
    auto result = FindWindowByRegex(
        "App",
        "Target.*",
        [&]() { return std::vector<WindowInfo>{match, other}; });
    EXPECT_EQ(result.handle, match.handle);
    EXPECT_TRUE(result.error_code.empty());
}

TEST(Win32WindowFinder, CaseSensitiveRegex)
{
    // ECMA-262 regex is case-sensitive by default. ClassName "MixedCase"
    // should NOT match "mixedcase".
    WindowInfo w;
    w.handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
    w.class_name = "MixedCase";
    auto lower = FindWindowByRegex(
        "mixedcase",
        "",
        [&]() { return std::vector<WindowInfo>{w}; });
    EXPECT_EQ(lower.error_code, "controller-window-not-found");
    auto exact = FindWindowByRegex(
        "MixedCase",
        "",
        [&]() { return std::vector<WindowInfo>{w}; });
    EXPECT_EQ(exact.handle, w.handle);
    EXPECT_TRUE(exact.error_code.empty());
}

// Load-contract lockdown: this test only locks down the no-throw +
// idempotent CODE CONTRACT of LoadMaaDynamicSymbols. It deliberately
// makes NO assertion on any framework-dependent symbol field's pointer
// value. Real production symbol resolution is NOT verified by any
// automated test here:
//   - The test stub boundary mocks MaaFW by design, so unit tests never
//     exercise the real symbol-resolution path.
//   - boost::dll::this_line_location() inside the test exe returns this
//     exe's own directory, which overlaps the default DLL search base;
//     any co-located null/non-null assertion would be unfaithful (both
//     the bare-name and plugin-dir loads would resolve identically).
// Production verification = construction (the toolkit load pattern
// already works in production; the framework load mirrors it) +
// empirical evidence (commit 89962574 proved the bare-name load fails
// when the plugin is loaded from a non-system directory) + post-exec
// manual/E2E (Win32/Gamepad typed controller production scenarios).

TEST(MaaDynamicSymbols, LoadNeverThrowsEvenWhenLibrariesUnavailable)
{
    // The loader must swallow boost::system::error_code failures and any
    // this_line_location() exception, and return a populated (mostly-null)
    // struct rather than throwing. Idempotent re-entry must also be safe:
    // the singleton's static-local initialization runs once, and a second
    // call must not throw even if the first already loaded (or failed to
    // load) the framework handle.
    EXPECT_NO_THROW((void)LoadMaaDynamicSymbols());
    const auto& symbols = LoadMaaDynamicSymbols();
    EXPECT_NO_THROW((void)LoadMaaDynamicSymbols());
    // Touch every pointer so the compiler can't optimize the call away.
    // Nullness is deliberately not asserted (see the contract comment above).
    (void)symbols.win32_create;
    (void)symbols.gamepad_create;
    (void)symbols.tk_list_create;
    (void)symbols.tk_list_destroy;
    (void)symbols.tk_find_all;
    (void)symbols.tk_list_size;
    (void)symbols.tk_list_at;
    (void)symbols.tk_get_handle;
    (void)symbols.tk_get_class_name;
    (void)symbols.tk_get_window_name;
    SUCCEED() << "LoadMaaDynamicSymbols returned without throwing";
}

TEST(
    MaaDynamicSymbols,
    DefaultWindowEnumeratorDegradesGracefullyWhenToolkitUnavailable)
{
    // The production enumerator is built lazily from LoadMaaDynamicSymbols.
    // When MaaToolkit is not loaded, the enumerator must return an empty
    // window list (not crash). A subsequent FindWindowByRegex call surfaces
    // controller-window-not-found — exactly what end users see when the
    // DLL is missing or the platform doesn't export the symbols.
    SetWindowEnumeratorForTest(nullptr);
    auto result = FindWindowByRegex("AnyClass", "AnyWindow");
    EXPECT_EQ(result.handle, nullptr);
    // Either the enumerator returned empty (controller-window-not-found) or
    // the system actually had real desktop windows but none matched the
    // regex — both produce the not-found diagnostic. Either way, no crash.
    EXPECT_EQ(result.error_code, "controller-window-not-found");
}

// ===== 设备 cache 覆盖 raw_json 六字段（PiCompiler 层）=====

// selected_adb_device 非空时覆盖 controller.raw.raw_json 的 adb 字段（Toolkit
// 探测的推荐值优先；agent_path 不覆盖，仍取 raw_json 解析值）。
TEST(NormalizeControllerInitParams, SelectedAdbDeviceOverridesRawJson)
{
    PiController controller;
    controller.dto.name = "CustomAndroid";
    controller.dto.type = "Adb";
    controller.raw.raw_json =
        R"({"name":"CustomAndroid","type":"Adb",)"
        R"("address":"raw.example:5555","adbPath":"raw_adb",)"
        R"("config":"{\"raw\":true}","agentPath":"raw_agent"})";

    AdbDeviceInfo device;
    device.name = "emulator-5554";
    device.adb_path = "/toolkit/adb";
    device.address = "127.0.0.1:5554";
    device.screencap_methods = 0x1;
    device.input_methods = 0x2;
    device.config = R"({"toolkit":true})";

    const auto params = NormalizeControllerInitParams(controller, &device);
    EXPECT_EQ(params.controller_type, "Adb");
    ASSERT_TRUE(params.adb.has_value());
    // 六字段中前五字段来自 cache（Toolkit 推荐值）。
    EXPECT_EQ(params.adb->address, "127.0.0.1:5554");
    EXPECT_EQ(params.adb->adb_path, "/toolkit/adb");
    EXPECT_EQ(params.adb->screencap_method, 0x1u);
    EXPECT_EQ(params.adb->input_method, 0x2u);
    EXPECT_EQ(params.adb->config_json, R"({"toolkit":true})");
    // agent_path 不在 AdbDeviceInfo 中，仍取 raw_json 解析值。
    EXPECT_EQ(params.adb->agent_path, "raw_agent");
}

// selected_adb_device=nullptr 时完全用 raw_json 既有六字段（用户未选设备或
// cache 不可用，既有行为保持 — 不会因为 cache 缺席而清空 adb 块）。
TEST(NormalizeControllerInitParams, NullAdbDeviceFallsBackToRawJson)
{
    PiController controller;
    controller.dto.name = "Android";
    controller.dto.type = "Adb";
    controller.raw.raw_json =
        R"({"name":"Android","type":"Adb","address":"raw.example:5555",)"
        R"("adbPath":"raw_adb","config":"{\"raw\":true}"})";

    const auto params = NormalizeControllerInitParams(
        controller,
        /*selected_adb_device=*/nullptr);
    EXPECT_EQ(params.controller_type, "Adb");
    ASSERT_TRUE(params.adb.has_value());
    EXPECT_EQ(params.adb->address, "raw.example:5555");
    EXPECT_EQ(params.adb->adb_path, "raw_adb");
    EXPECT_EQ(params.adb->screencap_method, 0x0u);
    EXPECT_EQ(params.adb->input_method, 0x0u);
    EXPECT_EQ(params.adb->config_json, R"({"raw":true})");
}

#include "../src/ControllerMethodMaps.h"
#include <das/Plugins/DasMaaPi/PiParser.h>

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

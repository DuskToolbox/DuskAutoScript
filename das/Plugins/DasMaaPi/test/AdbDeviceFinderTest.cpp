#include "AdbDeviceFinder.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using Das::Plugins::DasMaaPi::AdbDeviceInfo;
using Das::Plugins::DasMaaPi::DefaultAdbDeviceEnumerator;
using Das::Plugins::DasMaaPi::FindAllAdbDevices;
using Das::Plugins::DasMaaPi::SetAdbDeviceEnumeratorForTest;

// Injectable-enumerator contract for AdbDeviceFinder (mirrors the
// Win32WindowFinder test seam). Each TEST that installs a custom enumerator
// MUST clear it (SetAdbDeviceEnumeratorForTest(nullptr)) before returning
// so adjacent tests in the same TU start from the default state.

TEST(AdbDeviceFinder, NoMatchReturnsEmptyWhenEnumeratorReturnsEmpty)
{
    SetAdbDeviceEnumeratorForTest([]()
                                  { return std::vector<AdbDeviceInfo>{}; });
    const auto devices = FindAllAdbDevices();
    EXPECT_TRUE(devices.empty());
    SetAdbDeviceEnumeratorForTest(nullptr);
}

TEST(AdbDeviceFinder, SingleDeviceReturnsAllFiveFields)
{
    AdbDeviceInfo device;
    device.name = "emulator-5554";
    device.adb_path = "/usr/bin/adb";
    device.address = "127.0.0.1:5555";
    device.screencap_methods = 0x1;
    device.input_methods = 0x2;
    device.config = "{}";
    SetAdbDeviceEnumeratorForTest(
        [&device]() { return std::vector<AdbDeviceInfo>{device}; });
    const auto devices = FindAllAdbDevices();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].name, "emulator-5554");
    EXPECT_EQ(devices[0].adb_path, "/usr/bin/adb");
    EXPECT_EQ(devices[0].address, "127.0.0.1:5555");
    EXPECT_EQ(devices[0].screencap_methods, 0x1u);
    EXPECT_EQ(devices[0].input_methods, 0x2u);
    EXPECT_EQ(devices[0].config, "{}");
    SetAdbDeviceEnumeratorForTest(nullptr);
}

TEST(AdbDeviceFinder, MultipleDevicesAllEnumeratedAndOrderPreserved)
{
    AdbDeviceInfo first;
    first.name = "device-a";
    first.address = "127.0.0.1:5554";
    AdbDeviceInfo second;
    second.name = "device-b";
    second.address = "127.0.0.1:5556";
    AdbDeviceInfo third;
    third.name = "device-c";
    third.address = "127.0.0.1:5558";
    SetAdbDeviceEnumeratorForTest(
        [&]() { return std::vector<AdbDeviceInfo>{first, second, third}; });
    const auto devices = FindAllAdbDevices();
    ASSERT_EQ(devices.size(), 3u);
    EXPECT_EQ(devices[0].address, "127.0.0.1:5554");
    EXPECT_EQ(devices[1].address, "127.0.0.1:5556");
    EXPECT_EQ(devices[2].address, "127.0.0.1:5558");
    SetAdbDeviceEnumeratorForTest(nullptr);
}

TEST(AdbDeviceFinder, ToolkitUnavailableDegradesToEmpty)
{
    // In the DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY test build every adb_*
    // pointer in MaaDynamicSymbols is null (ResolveAll short-circuits with
    // the toolkit handle unloaded), so the production default enumerator
    // must return an empty list — not crash, not throw. This is the
    // graceful-degradation contract callers depend on when MaaToolkit.dll
    // is not co-located or the AdbDevice family is not exported.
    const auto enumerator = DefaultAdbDeviceEnumerator();
    const auto devices = enumerator();
    EXPECT_TRUE(devices.empty());
}

TEST(AdbDeviceFinder, RestoringEnumeratorReturnsToDefault)
{
    // Installing a custom enumerator must divert FindAllAdbDevices(), and
    // clearing the seam must restore default behavior. In the
    // DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY test build the default path
    // yields an empty list, so we can assert the restore by observing
    // empty output again.
    AdbDeviceInfo injected;
    injected.name = "only-device";
    injected.address = "127.0.0.1:5559";
    SetAdbDeviceEnumeratorForTest(
        [&injected]() { return std::vector<AdbDeviceInfo>{injected}; });
    auto with_seam = FindAllAdbDevices();
    ASSERT_EQ(with_seam.size(), 1u);
    EXPECT_EQ(with_seam[0].address, "127.0.0.1:5559");

    SetAdbDeviceEnumeratorForTest(nullptr);
    auto after_clear = FindAllAdbDevices();
    EXPECT_TRUE(after_clear.empty());
}

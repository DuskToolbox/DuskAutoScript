#include <das/Plugins/DasMaaPi/AdbDeviceFinder.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using Das::Plugins::DasMaaPi::AdbDeviceEnumerator;
using Das::Plugins::DasMaaPi::AdbDeviceInfo;
using Das::Plugins::DasMaaPi::DefaultAdbDeviceEnumerator;
using Das::Plugins::DasMaaPi::FindAllAdbDevices;

// FindAllAdbDevices takes the enumerator explicitly — no global test seam,
// no install/clear lifecycle. Each test passes its own lambda; production
// callers pass DefaultAdbDeviceEnumerator().

TEST(AdbDeviceFinder, EmptyYieldingEnumeratorReturnsEmpty)
{
    const auto devices =
        FindAllAdbDevices([]() { return std::vector<AdbDeviceInfo>{}; });
    EXPECT_TRUE(devices.empty());
}

TEST(AdbDeviceFinder, EmptyFunctionObjectReturnsEmpty)
{
    // A default-constructed (empty) AdbDeviceEnumerator short-circuits to an
    // empty result — the path a caller takes when no scan is wired.
    const auto devices = FindAllAdbDevices(AdbDeviceEnumerator{});
    EXPECT_TRUE(devices.empty());
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
    const auto devices = FindAllAdbDevices(
        [&device]() { return std::vector<AdbDeviceInfo>{device}; });
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].name, "emulator-5554");
    EXPECT_EQ(devices[0].adb_path, "/usr/bin/adb");
    EXPECT_EQ(devices[0].address, "127.0.0.1:5555");
    EXPECT_EQ(devices[0].screencap_methods, 0x1u);
    EXPECT_EQ(devices[0].input_methods, 0x2u);
    EXPECT_EQ(devices[0].config, "{}");
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
    const auto devices = FindAllAdbDevices(
        [&]() { return std::vector<AdbDeviceInfo>{first, second, third}; });
    ASSERT_EQ(devices.size(), 3u);
    EXPECT_EQ(devices[0].address, "127.0.0.1:5554");
    EXPECT_EQ(devices[1].address, "127.0.0.1:5556");
    EXPECT_EQ(devices[2].address, "127.0.0.1:5558");
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

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

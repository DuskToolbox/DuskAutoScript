#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    // One discovered ADB device. The five fields map 1:1 to the init params
    // of MaaAdbControllerCreate (adb_path / address / screencap / input /
    // config), plus `name` for UI display. `address` is the user-facing
    // selection key — it is the stable identity users pick from the dropdown
    // and persists into AcceptedSettings.controllerAdbDevice; the other four
    // are Toolkit-recommended values fetched at runtime from the device cache
    // (not persisted).
    struct AdbDeviceInfo
    {
        std::string   name;
        std::string   adb_path;
        std::string   address;
        std::uint64_t screencap_methods = 0;
        std::uint64_t input_methods = 0;
        std::string   config;
    };

    // Injectable device enumerator. Production DefaultAdbDeviceEnumerator
    // wraps the MaaToolkitAdbDevice* family; tests pass a lambda returning
    // a fixed vector to drive 0/1/N-device coverage without linking
    // MaaToolkit.
    using AdbDeviceEnumerator = std::function<std::vector<AdbDeviceInfo>()>;

    // Enumerate all ADB devices via the supplied enumerator. Thin pass-
    // through; kept as a named function so the production no-arg overload
    // and tests share a single resolution point.
    std::vector<AdbDeviceInfo> FindAllAdbDevices(
        const AdbDeviceEnumerator& enumerator);

    // Convenience overload using the production default enumerator
    // (MaaToolkitAdbDeviceFind). Under DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
    // (test build) every adb_* pointer is null and this returns an empty
    // list. Tests that need deterministic output call
    // SetAdbDeviceEnumeratorForTest first.
    std::vector<AdbDeviceInfo> FindAllAdbDevices();

    // Production enumerator backed by MaaToolkitAdbDeviceFind + the six
    // per-device getters. Degrades to an empty list when MaaToolkit is not
    // co-located or any required symbol is not exported — no exception,
    // no null dereference. Devices with an empty address are skipped
    // (Toolkit contract: address is the stable device identity).
    AdbDeviceEnumerator DefaultAdbDeviceEnumerator();

    // Test seam: install a custom enumerator used by the no-arg
    // FindAllAdbDevices overload. Pass nullptr to restore the default.
    // Tests must clear the seam before returning to avoid leaking the
    // override into adjacent tests in the same TU.
    void SetAdbDeviceEnumeratorForTest(AdbDeviceEnumerator enumerator);
} // namespace Das::Plugins::DasMaaPi

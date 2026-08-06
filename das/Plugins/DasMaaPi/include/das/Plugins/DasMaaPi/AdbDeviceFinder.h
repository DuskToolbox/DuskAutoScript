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
    // wraps the MaaToolkitAdbDevice* family; callers pass a custom enumerator
    // to drive 0/1/N-device coverage without linking MaaToolkit. Passed by
    // value through call sites — a default-constructed (empty) instance is
    // treated as "use the production default".
    using AdbDeviceEnumerator = std::function<std::vector<AdbDeviceInfo>()>;

    // Whether the loaded MaaToolkit exports the full AdbDevice symbol set.
    // discoverAdbDevices uses this to distinguish "toolkit unavailable"
    // (warning) from "scan succeeded, zero devices" (info). False when the
    // DLL is not co-located or any required symbol is not exported.
    bool IsAdbToolkitAvailable();

    // Enumerate all ADB devices via the supplied enumerator. Thin pass-
    // through; production callers pass DefaultAdbDeviceEnumerator(), tests
    // pass a lambda returning a fixed vector.
    std::vector<AdbDeviceInfo> FindAllAdbDevices(
        const AdbDeviceEnumerator& enumerator);

    // Production enumerator backed by MaaToolkitAdbDeviceFind + the six
    // per-device getters. Degrades to an empty list when MaaToolkit is not
    // co-located or any required symbol is not exported — no exception,
    // no null dereference. Devices with an empty address are skipped
    // (Toolkit contract: address is the stable device identity).
    AdbDeviceEnumerator DefaultAdbDeviceEnumerator();
} // namespace Das::Plugins::DasMaaPi

#include "AdbDeviceFinder.h"

#include "MaaDynamicSymbols.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        AdbDeviceEnumerator g_enumerator_for_test;
    } // namespace

    std::vector<AdbDeviceInfo> FindAllAdbDevices(
        const AdbDeviceEnumerator& enumerator)
    {
        if (!enumerator)
        {
            return {};
        }
        return enumerator();
    }

    AdbDeviceEnumerator DefaultAdbDeviceEnumerator()
    {
        return []() -> std::vector<AdbDeviceInfo>
        {
            // MaaToolkit is loaded at runtime via MaaDynamicSymbols (one
            // cross-platform binary; no #ifdef
            // DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY branch needed). When the
            // DLL is not co-located or any of the device-discovery symbols
            // is not exported, every pointer stays null and we return an
            // empty list — the caller surfaces an empty device dropdown,
            // exactly as if no ADB device was attached.
            //
            // adb_find_specified is intentionally not required: the default
            // enumerator uses the unspecific MaaToolkitAdbDeviceFind that
            // walks adb_server's full device list. find_specified is bound
            // on the symbol table for future callers but not gated here.
            const auto& symbols = LoadMaaDynamicSymbols();
            if (!symbols.adb_list_create || !symbols.adb_list_destroy
                || !symbols.adb_find || !symbols.adb_list_size
                || !symbols.adb_list_at || !symbols.adb_get_name
                || !symbols.adb_get_adb_path || !symbols.adb_get_address
                || !symbols.adb_get_screencap_methods
                || !symbols.adb_get_input_methods || !symbols.adb_get_config)
            {
                return {};
            }

            // MaaToolkitAdbDeviceFind owns the returned list handle; we
            // copy out the per-device fields then destroy it before
            // returning. Custom deleter routes through the runtime-resolved
            // destroy symbol (stateful — holds the function pointer, since
            // unique_ptr's deleter slot is a function-pointer type and a
            // capturing lambda cannot convert to it).
            struct ToolkitListDeleter
            {
                MaaToolkitAdbDeviceListDestroyFn destroy;
                void operator()(void* p) const noexcept
                {
                    if (p && destroy)
                    {
                        destroy(p);
                    }
                }
            };
            const std::unique_ptr<void, ToolkitListDeleter> list(
                symbols.adb_list_create(),
                ToolkitListDeleter{symbols.adb_list_destroy});
            if (!list)
            {
                return {};
            }
            // MaaToolkitAdbDeviceFind returns MaaBool: nonzero on success,
            // zero on failure (adb_server unreachable, etc.). Treat zero as
            // "no devices" — the caller's dropdown stays empty rather than
            // crashing.
            if (!symbols.adb_find(list.get()))
            {
                return {};
            }
            std::vector<AdbDeviceInfo> devices;
            const auto                 size = symbols.adb_list_size(list.get());
            for (std::uint64_t i = 0; i < size; ++i)
            {
                const auto* device = symbols.adb_list_at(list.get(), i);
                if (!device)
                {
                    continue;
                }
                AdbDeviceInfo info;
                if (const char* name = symbols.adb_get_name(device))
                {
                    info.name = name;
                }
                if (const char* adb_path = symbols.adb_get_adb_path(device))
                {
                    info.adb_path = adb_path;
                }
                if (const char* address = symbols.adb_get_address(device))
                {
                    info.address = address;
                }
                // address is the user-facing selection key and the cache
                // lookup key in PiCompiler; an empty address means the
                // Toolkit yielded garbage we cannot reconcile later, so
                // skip the entry entirely.
                if (info.address.empty())
                {
                    continue;
                }
                info.screencap_methods =
                    symbols.adb_get_screencap_methods(device);
                info.input_methods = symbols.adb_get_input_methods(device);
                if (const char* config = symbols.adb_get_config(device))
                {
                    info.config = config;
                }
                devices.emplace_back(std::move(info));
            }
            return devices;
        };
    }

    void SetAdbDeviceEnumeratorForTest(AdbDeviceEnumerator enumerator)
    {
        if (enumerator)
        {
            g_enumerator_for_test = std::move(enumerator);
        }
        else
        {
            g_enumerator_for_test = nullptr;
        }
    }

    std::vector<AdbDeviceInfo> FindAllAdbDevices()
    {
        const auto& enumerator = g_enumerator_for_test
                                     ? g_enumerator_for_test
                                     : DefaultAdbDeviceEnumerator();
        return FindAllAdbDevices(enumerator);
    }
} // namespace Das::Plugins::DasMaaPi

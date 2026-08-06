#pragma once

#include <cstdint>

namespace Das::Plugins::DasMaaPi
{
    // Runtime-resolved function pointers for the platform-specific MaaFW /
    // MaaToolkit symbols. Replacing the previous hard-link + #ifdef sprawl
    // with boost::dll runtime lookup lets one cross-platform DasMaaPi binary
    // adapt to the host: Windows exports MaaWin32ControllerCreate /
    // MaaGamepadControllerCreate / the MaaToolkit window-enumeration family;
    // other platforms do not, and the missing symbols degrade to
    // controller-type-unsupported-on-platform instead of failing at link
    // time.
    //
    // The typedefs deliberately use opaque types (void* / std::uint64_t) so
    // the header compiles in BOTH the production target (MaaFW headers
    // visible) AND the DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY test target
    // (MaaFW headers invisible). The on-disk C signatures they mirror live
    // in MaaFramework/Instance/MaaController.h and
    // MaaToolkit/DesktopWindow/MaaToolkitDesktopWindow.h. Sizes are fixed by
    // MaaDef.h: MaaBool = uint8_t, MaaSize = uint64_t,
    // MaaWin32ScreencapMethod / MaaWin32InputMethod / MaaGamepadType =
    // uint64_t. Calling convention is cdecl (no MAA_CALL on these symbols).

    // MaaController* MaaWin32ControllerCreate(
    //     void* hWnd, MaaWin32ScreencapMethod, MaaWin32InputMethod mouse,
    //     MaaWin32InputMethod keyboard);
    using MaaWin32ControllerCreateFn = void* (*)(void*,
                                                 std::uint64_t,
                                                 std::uint64_t,
                                                 std::uint64_t);

    // MaaController* MaaGamepadControllerCreate(
    //     void* hWnd, MaaGamepadType, MaaWin32ScreencapMethod);
    using MaaGamepadControllerCreateFn = void* (*)(void*,
                                                   std::uint64_t,
                                                   std::uint64_t);

    // MaaToolkitDesktopWindowList* MaaToolkitDesktopWindowListCreate();
    using MaaToolkitDesktopWindowListCreateFn = void* (*)();

    // void MaaToolkitDesktopWindowListDestroy(MaaToolkitDesktopWindowList*);
    using MaaToolkitDesktopWindowListDestroyFn = void (*)(void*);

    // MaaBool MaaToolkitDesktopWindowFindAll(MaaToolkitDesktopWindowList*);
    using MaaToolkitDesktopWindowFindAllFn = std::uint8_t (*)(void*);

    // MaaSize MaaToolkitDesktopWindowListSize(const
    // MaaToolkitDesktopWindowList*);
    using MaaToolkitDesktopWindowListSizeFn = std::uint64_t (*)(const void*);

    // const MaaToolkitDesktopWindow* MaaToolkitDesktopWindowListAt(
    //     const MaaToolkitDesktopWindowList*, MaaSize);
    using MaaToolkitDesktopWindowListAtFn = const void* (*)(const void*,
                                                            std::uint64_t);

    // void* MaaToolkitDesktopWindowGetHandle(const MaaToolkitDesktopWindow*);
    using MaaToolkitDesktopWindowGetHandleFn = void* (*)(const void*);

    // const char* MaaToolkitDesktopWindowGetClassName(
    //     const MaaToolkitDesktopWindow*);
    using MaaToolkitDesktopWindowGetClassNameFn = const char* (*)(const void*);

    // const char* MaaToolkitDesktopWindowGetWindowName(
    //     const MaaToolkitDesktopWindow*);
    using MaaToolkitDesktopWindowGetWindowNameFn = const char* (*)(const void*);

    // MaaToolkitAdbDevice* family — ADB device discovery + recommended
    // init-param probe. Same runtime-resolution pattern as the desktop-window
    // family above: resolved from the co-located MaaToolkit.dll, opaque
    // typedefs so the header compiles without MaaFW includes (mirrors the
    // tk_* block). On-disk C signatures live in
    // MaaToolkit/AdbDevice/MaaToolkitAdbDevice.h. MaaBool = uint8_t,
    // MaaSize = uint64_t, MaaAdbScreencapMethod / MaaAdbInputMethod =
    // uint64_t (MaaDef.h).

    // MaaToolkitAdbDeviceList* MaaToolkitAdbDeviceListCreate();
    using MaaToolkitAdbDeviceListCreateFn = void* (*)();

    // void MaaToolkitAdbDeviceListDestroy(MaaToolkitAdbDeviceList*);
    using MaaToolkitAdbDeviceListDestroyFn = void (*)(void*);

    // MaaBool MaaToolkitAdbDeviceFind(MaaToolkitAdbDeviceList* buffer);
    using MaaToolkitAdbDeviceFindFn = std::uint8_t (*)(void*);

    // MaaBool MaaToolkitAdbDeviceFindSpecified(
    //     const char* adb_path, MaaToolkitAdbDeviceList* buffer);
    using MaaToolkitAdbDeviceFindSpecifiedFn =
        std::uint8_t (*)(const char*, void*);

    // MaaSize MaaToolkitAdbDeviceListSize(const MaaToolkitAdbDeviceList*);
    using MaaToolkitAdbDeviceListSizeFn = std::uint64_t (*)(const void*);

    // const MaaToolkitAdbDevice* MaaToolkitAdbDeviceListAt(
    //     const MaaToolkitAdbDeviceList*, MaaSize index);
    using MaaToolkitAdbDeviceListAtFn = const void* (*)(const void*,
                                                        std::uint64_t);

    // const char* MaaToolkitAdbDeviceGetName(const MaaToolkitAdbDevice*);
    using MaaToolkitAdbDeviceGetNameFn = const char* (*)(const void*);

    // const char* MaaToolkitAdbDeviceGetAdbPath(const MaaToolkitAdbDevice*);
    using MaaToolkitAdbDeviceGetAdbPathFn = const char* (*)(const void*);

    // const char* MaaToolkitAdbDeviceGetAddress(const MaaToolkitAdbDevice*);
    using MaaToolkitAdbDeviceGetAddressFn = const char* (*)(const void*);

    // MaaAdbScreencapMethod MaaToolkitAdbDeviceGetScreencapMethods(
    //     const MaaToolkitAdbDevice*);
    using MaaToolkitAdbDeviceGetScreencapMethodsFn =
        std::uint64_t (*)(const void*);

    // MaaAdbInputMethod MaaToolkitAdbDeviceGetInputMethods(
    //     const MaaToolkitAdbDevice*);
    using MaaToolkitAdbDeviceGetInputMethodsFn = std::uint64_t (*)(const void*);

    // const char* MaaToolkitAdbDeviceGetConfig(const MaaToolkitAdbDevice*);
    using MaaToolkitAdbDeviceGetConfigFn = const char* (*)(const void*);

    // All symbols routed through runtime lookup. Any
    // pointer that fails to resolve stays null — callers must check before
    // dispatching and surface controller-type-unsupported-on-platform (or an
    // empty window list) instead of dereferencing.
    //
    // MaaFW framework version 不在此处解析：MaaVersion() 是 MaaFramework 通用
    // 跨平台 API，由 AuthoringProjector 硬链直接调用（与 PiCompiler
    // QueryMaaFrameworkVersion 同源），不经动态符号。
    struct MaaDynamicSymbols
    {
        MaaWin32ControllerCreateFn               win32_create = nullptr;
        MaaGamepadControllerCreateFn             gamepad_create = nullptr;
        MaaToolkitDesktopWindowListCreateFn      tk_list_create = nullptr;
        MaaToolkitDesktopWindowListDestroyFn     tk_list_destroy = nullptr;
        MaaToolkitDesktopWindowFindAllFn         tk_find_all = nullptr;
        MaaToolkitDesktopWindowListSizeFn        tk_list_size = nullptr;
        MaaToolkitDesktopWindowListAtFn          tk_list_at = nullptr;
        MaaToolkitDesktopWindowGetHandleFn       tk_get_handle = nullptr;
        MaaToolkitDesktopWindowGetClassNameFn    tk_get_class_name = nullptr;
        MaaToolkitDesktopWindowGetWindowNameFn   tk_get_window_name = nullptr;
        MaaToolkitAdbDeviceListCreateFn          adb_list_create = nullptr;
        MaaToolkitAdbDeviceListDestroyFn         adb_list_destroy = nullptr;
        MaaToolkitAdbDeviceFindFn                adb_find = nullptr;
        MaaToolkitAdbDeviceFindSpecifiedFn       adb_find_specified = nullptr;
        MaaToolkitAdbDeviceListSizeFn            adb_list_size = nullptr;
        MaaToolkitAdbDeviceListAtFn              adb_list_at = nullptr;
        MaaToolkitAdbDeviceGetNameFn             adb_get_name = nullptr;
        MaaToolkitAdbDeviceGetAdbPathFn          adb_get_adb_path = nullptr;
        MaaToolkitAdbDeviceGetAddressFn          adb_get_address = nullptr;
        MaaToolkitAdbDeviceGetScreencapMethodsFn adb_get_screencap_methods =
            nullptr;
        MaaToolkitAdbDeviceGetInputMethodsFn adb_get_input_methods = nullptr;
        MaaToolkitAdbDeviceGetConfigFn       adb_get_config = nullptr;
    };

    // Singleton accessor: the first call resolves every symbol via
    // boost::dll and caches the result for the process lifetime. The
    // underlying boost::dll::shared_library handles are kept alive inside
    // the singleton (RAII: destructors run at static destruction time, no
    // leak, ASAN-clean). Resolution is best-effort:
    //   - MaaFramework: already loaded into the process (hard-linked for
    //     cross-platform core symbols); Win32/Gamepad create symbols are
    //     resolved from that already-loaded module.
    //   - MaaToolkit: freshly loaded from the plugin's own co-located
    //     directory (MaaToolkit.dll is copied there at build time; CppHost
    //     loads the plugin with load_with_altered_search_path so sibling
    //     deps resolve from that dir).
    const MaaDynamicSymbols& LoadMaaDynamicSymbols();
} // namespace Das::Plugins::DasMaaPi

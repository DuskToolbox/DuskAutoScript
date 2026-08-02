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

    // MaaSinkId MaaTaskerAddSink(MaaTasker* tasker, MaaEventCallback sink,
    //     void* trans_arg);
    // The sink callback is a MaaEventCallback (__stdcall on Windows); it is
    // passed opaquely as void* here so the header stays free of MaaFW
    // includes. Return is MaaSinkId (int64_t), 0 == MaaInvalidId.
    using MaaTaskerAddSinkFn = std::int64_t (*)(void*, void*, void*);

    // const char* MaaVersion();
    // MaaFW framework version (D-89-07 修订): version-display data source.
    // Returns a pointer to a static string owned by MaaFramework; callers must
    // copy it into std::string immediately before it can go stale. C-signature
    // opaque typedef so the header needs no MaaFW include.
    using MaaVersionFn = const char* (*)();

    // All symbols routed through runtime lookup. Any
    // pointer that fails to resolve stays null — callers must check before
    // dispatching and surface controller-type-unsupported-on-platform (or an
    // empty window list) instead of dereferencing.
    struct MaaDynamicSymbols
    {
        MaaTaskerAddSinkFn                     tasker_add_sink = nullptr;
        MaaWin32ControllerCreateFn             win32_create = nullptr;
        MaaGamepadControllerCreateFn           gamepad_create = nullptr;
        MaaToolkitDesktopWindowListCreateFn    tk_list_create = nullptr;
        MaaToolkitDesktopWindowListDestroyFn   tk_list_destroy = nullptr;
        MaaToolkitDesktopWindowFindAllFn       tk_find_all = nullptr;
        MaaToolkitDesktopWindowListSizeFn      tk_list_size = nullptr;
        MaaToolkitDesktopWindowListAtFn        tk_list_at = nullptr;
        MaaToolkitDesktopWindowGetHandleFn     tk_get_handle = nullptr;
        MaaToolkitDesktopWindowGetClassNameFn  tk_get_class_name = nullptr;
        MaaToolkitDesktopWindowGetWindowNameFn tk_get_window_name = nullptr;

        // MaaFW framework version (MaaVersion). null when MaaFramework is not
        // mapped or the symbol is absent — callers degrade to a hidden
        // version display. Data source for 89-03 authoring field projection
        // (D-89-07 修订; no dedicated GetVersionInfo query channel).
        MaaVersionFn version = nullptr;
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

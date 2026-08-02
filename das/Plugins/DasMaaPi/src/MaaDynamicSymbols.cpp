#include "MaaDynamicSymbols.h"

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/dll/shared_library.hpp>
#include <boost/system/error_code.hpp>

#include <exception>
#include <utility>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        // Owns the shared_library handles so the loaded code stays mapped
        // for the process lifetime. Function pointers in MaaDynamicSymbols
        // point into these mappings; if the libraries were allowed to
        // unload, every call site would UAF. Held as a static-local inside
        // LoadMaaDynamicSymbols so initialization order is well-defined
        // and destruction runs only at shutdown.
        struct LoadedLibraryHandles
        {
            boost::dll::shared_library framework;
            boost::dll::shared_library toolkit;
        };

        template <typename Signature>
        Signature TryResolve(
            const boost::dll::shared_library& lib,
            const char*                       symbol_name) noexcept
        {
            // has() + get<>() would be racy if the library could unload
            // between the two calls, but the library is held alive by the
            // LoadedLibraryHandles singleton. Load-time resolution only.
            if (!lib.has(symbol_name))
            {
                return nullptr;
            }
            try
            {
                return lib.get<Signature>(symbol_name);
            }
            catch (const std::exception&)
            {
                // get<>() throws on type/signature mismatch; treat as
                // unavailable so the caller degrades to
                // controller-type-unsupported-on-platform.
                return nullptr;
            }
        }

        MaaDynamicSymbols ResolveAll(LoadedLibraryHandles& handles)
        {
            MaaDynamicSymbols         symbols;
            boost::system::error_code ec;

            // MaaFramework.dll is hard-linked for the cross-platform core
            // (MaaAdbControllerCreate / MaaDbgControllerCreate / ...), so
            // it is already mapped into the process by the time DasMaaPi
            // runs. Loading it by name returns the existing module handle
            // (Windows refcounts module handles; *nix dlopen semantics
            // match). On a platform whose MaaFW build does not export
            // MaaWin32ControllerCreate / MaaGamepadControllerCreate (Linux
            // MaaFW has no Win32/Gamepad controller), has() returns false
            // and the pointer stays null — exactly the dynamic-failure
            // behavior we want, with zero #ifdef sprawl.
            handles.framework.load(
                "MaaFramework",
                boost::dll::load_mode::search_system_folders,
                ec);
            if (!ec && handles.framework.is_loaded())
            {
                // MaaTaskerAddSink registers the MaaEventCallback sink
                // (D-88-07). Resolved from the already-mapped MaaFramework
                // module like the create symbols; null → MaaRuntime
                // SetCallback returns a Failure diagnostic (T-88-13).
                symbols.tasker_add_sink = TryResolve<MaaTaskerAddSinkFn>(
                    handles.framework,
                    "MaaTaskerAddSink");
                symbols.win32_create = TryResolve<MaaWin32ControllerCreateFn>(
                    handles.framework,
                    "MaaWin32ControllerCreate");
                symbols.gamepad_create =
                    TryResolve<MaaGamepadControllerCreateFn>(
                        handles.framework,
                        "MaaGamepadControllerCreate");
                // MaaFW framework version — version-display data source
                // (D-89-07 修订). null-safe: MaaFW absent / symbol missing →
                // nullptr, callers degrade.
                symbols.version =
                    TryResolve<MaaVersionFn>(handles.framework, "MaaVersion");
            }

            // MaaToolkit is NOT hard-linked (after this refactor), so a
            // fresh load is required. The DLL is co-located with the
            // DasMaaPi plugin (CMakeLists.txt POST_BUILD copies every
            // DAS_MAAFRAMEWORK_RUNTIME_DLLS — including MaaToolkit.dll —
            // into $<TARGET_FILE_DIR:DasMaaPi>). Derive that directory
            // from the current module's path so the lookup is independent
            // of the process working directory and any PATH assumptions.
            // load_with_altered_search_path makes Windows search for
            // MaaToolkit's own deps (MaaFramework, etc.) in the same dir.
            try
            {
                const auto plugin_module = boost::dll::this_line_location();
                const auto plugin_dir = plugin_module.parent_path();
                handles.toolkit.load(
                    plugin_dir / "MaaToolkit",
                    boost::dll::load_mode::load_with_altered_search_path,
                    ec);
            }
            catch (const std::exception&)
            {
                // this_line_location() can throw on exotic platforms; the
                // toolkit handles stay unloaded and all tk_* pointers
                // remain null.
            }
            if (!ec && handles.toolkit.is_loaded())
            {
                symbols.tk_list_create =
                    TryResolve<MaaToolkitDesktopWindowListCreateFn>(
                        handles.toolkit,
                        "MaaToolkitDesktopWindowListCreate");
                symbols.tk_list_destroy =
                    TryResolve<MaaToolkitDesktopWindowListDestroyFn>(
                        handles.toolkit,
                        "MaaToolkitDesktopWindowListDestroy");
                symbols.tk_find_all =
                    TryResolve<MaaToolkitDesktopWindowFindAllFn>(
                        handles.toolkit,
                        "MaaToolkitDesktopWindowFindAll");
                symbols.tk_list_size =
                    TryResolve<MaaToolkitDesktopWindowListSizeFn>(
                        handles.toolkit,
                        "MaaToolkitDesktopWindowListSize");
                symbols.tk_list_at =
                    TryResolve<MaaToolkitDesktopWindowListAtFn>(
                        handles.toolkit,
                        "MaaToolkitDesktopWindowListAt");
                symbols.tk_get_handle =
                    TryResolve<MaaToolkitDesktopWindowGetHandleFn>(
                        handles.toolkit,
                        "MaaToolkitDesktopWindowGetHandle");
                symbols.tk_get_class_name =
                    TryResolve<MaaToolkitDesktopWindowGetClassNameFn>(
                        handles.toolkit,
                        "MaaToolkitDesktopWindowGetClassName");
                symbols.tk_get_window_name =
                    TryResolve<MaaToolkitDesktopWindowGetWindowNameFn>(
                        handles.toolkit,
                        "MaaToolkitDesktopWindowGetWindowName");
            }

            return symbols;
        }
    } // namespace

    const MaaDynamicSymbols& LoadMaaDynamicSymbols()
    {
        static LoadedLibraryHandles    handles;
        static const MaaDynamicSymbols symbols = ResolveAll(handles);
        return symbols;
    }
} // namespace Das::Plugins::DasMaaPi

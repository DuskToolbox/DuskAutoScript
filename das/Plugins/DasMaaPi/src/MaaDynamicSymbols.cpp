#include "MaaDynamicSymbols.h"

#include <das/DasApi.h>
#include <das/Utils/fmt.h>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/dll/shared_library.hpp>
#include <boost/system/error_code.hpp>

#include <exception>
#include <type_traits>
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
            if (!lib.has(symbol_name))
            {
                return nullptr;
            }
            try
            {
                // boost::dll shared_library::get<T> 返回 T&（数据语义：把符号
                // 地址当存储 T 的内存，*reinterpret_cast<T*>(addr)）。对函数
                // 符号，T 必须是函数类型（如 void*()）而非函数指针类型
                // （void*(*)()）：get<void*()> 走
                // *reinterpret_cast<void*(*)()>(F) 把函数入口 F
                // 当函数指针解一层引用、返回绑定到 F 的函数引用，
                // 转函数指针即真实地址；若误用函数指针类型作 T，会再多解一层，
                // 读 F 处机器码当指针（实测 adb_list_create=0x20b928ec8348 =
                // prologue 指令字节，indirect call 即 execute
                // fault）。Signature 是函数指针，取 remove_pointer
                // 得函数类型喂给 get。
                using FuncType = std::remove_pointer_t<Signature>;
                return lib.template get<FuncType>(symbol_name);
            }
            catch (const std::exception&)
            {
                // get<>() throws on missing symbol / type mismatch; degrade
                // to unavailable so callers skip the feature gracefully.
                return nullptr;
            }
        }

        MaaDynamicSymbols ResolveAll(LoadedLibraryHandles& handles)
        {
            MaaDynamicSymbols         symbols;
            boost::system::error_code ec;

            // MaaFramework.dll is hard-linked for the cross-platform core
            // (MaaAdbControllerCreate / MaaDbgControllerCreate / the
            // PostConnection/Wait/Connected + sink family), so it is
            // already mapped into the process by the time DasMaaPi runs.
            // Those cross-platform core symbols are hard-linked direct
            // calls in ScopedMaaApiBoundary prod; this dynamic load serves
            // only the remaining platform-specific symbols (win32_create /
            // gamepad_create + the MaaToolkit family resolved below).
            //
            // The framework handle MUST be re-derived from the plugin's
            // own co-located directory, mirroring the toolkit load below:
            // bare-name load("MaaFramework") searches only system folders
            // and fails when DasHostX loads the plugin from a non-system
            // plugin directory, leaving every framework symbol null
            // (commit 89962574 documented the MaaVersion-blank symptom).
            // this_line_location().parent_path() + altered_search_path
            // returns the already-mapped module handle (Windows refcounts
            // module handles; already-mapped modules match by name
            // regardless of path). On a platform whose MaaFW build does
            // not export MaaWin32ControllerCreate /
            // MaaGamepadControllerCreate (Linux MaaFW has no Win32/Gamepad
            // controller), has() returns false and the pointer stays null
            // — the dynamic-failure behavior we want, with zero #ifdef
            // sprawl.
            try
            {
                const auto plugin_module = boost::dll::this_line_location();
                const auto plugin_dir = plugin_module.parent_path();
                handles.framework.load(
                    plugin_dir / "MaaFramework",
                    boost::dll::load_mode::append_decorations
                        | boost::dll::load_mode::load_with_altered_search_path,
                    ec);
            }
            catch (const std::exception&)
            {
                // this_line_location() can throw on exotic platforms; the
                // framework handle stays unloaded and every dynamic symbol
                // resolved below remains null.
            }
            if (!ec && handles.framework.is_loaded())
            {
                symbols.win32_create = TryResolve<MaaWin32ControllerCreateFn>(
                    handles.framework,
                    "MaaWin32ControllerCreate");
                symbols.gamepad_create =
                    TryResolve<MaaGamepadControllerCreateFn>(
                        handles.framework,
                        "MaaGamepadControllerCreate");
            }

            // MaaToolkit is NOT hard-linked (after this refactor), so a
            // fresh load is required. The DLL is co-located with the
            // DasMaaPi plugin (CMakeLists.txt POST_BUILD copies every
            // DAS_MAAFRAMEWORK_RUNTIME_DLLS — including MaaToolkit.dll —
            // into $<TARGET_FILE_DIR:DasMaaPi>). Derive that directory
            // from the current module's path so the lookup is independent
            // of the process working directory and any PATH assumptions.
            try
            {
                const auto plugin_module = boost::dll::this_line_location();
                const auto plugin_dir = plugin_module.parent_path();
                // 必须 load_with_altered_search_path +
                // append_decorations。两个坑： (1) altered：MaaToolkit
                // 的递归依赖（MaaFramework→fastdeploy/ onnxruntime_maa 等在
                // plugins/DasMaaPi/）需从 plugin_dir 搜索； DasHostX 的 PATH
                // 不含插件目录，默认搜索（System32+PATH）找不到， 故必须
                // altered 让依赖从 DLL 所在目录解析。 (2)
                // append_decorations：boost::dll 对无扩展 path 默认会附尾点
                // （"MaaToolkit."），LoadLibraryExW 把尾点当空扩展名 →
                // 找不到文件。 append_decorations 让 boost::dll 走
                // load_impl（LoadLibraryExW 直接收 decorated = 平台扩展后的
                // MaaToolkit.dll/.so/.dylib + lib 前缀），
                // 绕过尾点，且跨平台（不硬编码 .dll）。
                handles.toolkit.load(
                    plugin_dir / "MaaToolkit",
                    boost::dll::load_mode::append_decorations
                        | boost::dll::load_mode::load_with_altered_search_path,
                    ec);
                // 诊断：discoverAdbDevices 依赖 toolkit 的 11 个 AdbDevice
                // 符号族 （IsAdbToolkitAvailable 要求全 resolved）。load
                // 失败原本静默—— 操作员只看到 warning，记录 plugin_dir + ec
                // 暴露运行时实况。
                if (handles.toolkit.is_loaded())
                {
                    DAS_LOG_INFO(
                        DAS_FMT_NS::format(
                            "[DasMaaPi] MaaToolkit loaded from {}",
                            plugin_dir.string())
                            .c_str());
                }
                else
                {
                    DAS_LOG_WARNING(
                        DAS_FMT_NS::format(
                            "[DasMaaPi] MaaToolkit load failed: "
                            "dir={} ec={} msg={}",
                            plugin_dir.string(),
                            ec.value(),
                            ec.message())
                            .c_str());
                }
            }
            catch (const std::exception& ex)
            {
                // this_line_location() can throw on exotic platforms; the
                // toolkit handles stay unloaded and all tk_* pointers
                // remain null.
                DAS_LOG_WARNING(
                    DAS_FMT_NS::format(
                        "[DasMaaPi] MaaToolkit load threw: {}",
                        ex.what())
                        .c_str());
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

                // MaaToolkitAdbDevice* family — ADB device discovery +
                // recommended screencap/input/config probe. Same toolkit
                // module as the desktop-window family above, so resolved
                // in the same is_loaded() branch. adb_find_specified is
                // bound for completeness but not consumed by the default
                // enumerator (FindAllAdbDevices uses the unspecific Find).
                symbols.adb_list_create =
                    TryResolve<MaaToolkitAdbDeviceListCreateFn>(
                        handles.toolkit,
                        "MaaToolkitAdbDeviceListCreate");
                symbols.adb_list_destroy =
                    TryResolve<MaaToolkitAdbDeviceListDestroyFn>(
                        handles.toolkit,
                        "MaaToolkitAdbDeviceListDestroy");
                symbols.adb_find = TryResolve<MaaToolkitAdbDeviceFindFn>(
                    handles.toolkit,
                    "MaaToolkitAdbDeviceFind");
                symbols.adb_find_specified =
                    TryResolve<MaaToolkitAdbDeviceFindSpecifiedFn>(
                        handles.toolkit,
                        "MaaToolkitAdbDeviceFindSpecified");
                symbols.adb_list_size =
                    TryResolve<MaaToolkitAdbDeviceListSizeFn>(
                        handles.toolkit,
                        "MaaToolkitAdbDeviceListSize");
                symbols.adb_list_at = TryResolve<MaaToolkitAdbDeviceListAtFn>(
                    handles.toolkit,
                    "MaaToolkitAdbDeviceListAt");
                symbols.adb_get_name = TryResolve<MaaToolkitAdbDeviceGetNameFn>(
                    handles.toolkit,
                    "MaaToolkitAdbDeviceGetName");
                symbols.adb_get_adb_path =
                    TryResolve<MaaToolkitAdbDeviceGetAdbPathFn>(
                        handles.toolkit,
                        "MaaToolkitAdbDeviceGetAdbPath");
                symbols.adb_get_address =
                    TryResolve<MaaToolkitAdbDeviceGetAddressFn>(
                        handles.toolkit,
                        "MaaToolkitAdbDeviceGetAddress");
                symbols.adb_get_screencap_methods =
                    TryResolve<MaaToolkitAdbDeviceGetScreencapMethodsFn>(
                        handles.toolkit,
                        "MaaToolkitAdbDeviceGetScreencapMethods");
                symbols.adb_get_input_methods =
                    TryResolve<MaaToolkitAdbDeviceGetInputMethodsFn>(
                        handles.toolkit,
                        "MaaToolkitAdbDeviceGetInputMethods");
                symbols.adb_get_config =
                    TryResolve<MaaToolkitAdbDeviceGetConfigFn>(
                        handles.toolkit,
                        "MaaToolkitAdbDeviceGetConfig");
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

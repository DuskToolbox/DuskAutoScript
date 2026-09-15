#ifdef DAS_EXPORT_LUA

#include "LuaHost.h"

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_SOL2_WARNING
// clang-format off
#include <das/Core/ForeignInterfaceHost/LuaHeaders.h>
#include <sol/sol.hpp>
// clang-format on
DAS_DISABLE_WARNING_END

#include <boost/dll.hpp>
#include <cstdint>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/BoostPathBridge.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <filesystem>
#include <fstream>
#include <lauxlib.h>
#include <lua.h>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef DAS_LUA_OPEN_FUNCTION_NAME
#error                                                                         \
    "DAS_LUA_OPEN_FUNCTION_NAME must be defined when DAS_EXPORT_LUA is enabled"
#endif

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_LUAHOST_BEGIN

namespace
{
    // Lua C module open function signature
    using LuaOpenFunction = int(lua_State*);

    // DLL filename for the Lua export library
    constexpr const char* LUA_EXPORT_DLL_NAME = "DasCoreLuaExport.dll";
    // MinGW prefixes shared libraries with "lib"
    constexpr const char* LUA_EXPORT_DLL_NAME_MINGW = "libDasCoreLuaExport.dll";

    // luaopen function symbol name, supplied by CMake from the generator input.
    constexpr const char* LUA_OPEN_FUNCTION_NAME = DAS_LUA_OPEN_FUNCTION_NAME;

    // ExtractDasBasePointer helper function symbol name
    constexpr const char* EXTRACT_BASE_POINTER_NAME = "ExtractDasBasePointer";

    /// Try to find a DLL by name in the given directory
    auto TryDllInDir(const std::filesystem::path& dir, const char* dll_name)
        -> std::optional<std::filesystem::path>
    {
        auto dll_path = dir / Das::Utils::PathFromUtf8(dll_name);
        if (std::filesystem::exists(dll_path))
        {
            return dll_path;
        }
        return std::nullopt;
    }

    /// Find DasCoreLuaExport.dll — search manifest directory, then exe
    /// directory. On MinGW, try libDasCoreLuaExport.dll first.
    auto FindLuaExportDll(const std::filesystem::path& manifest_path)
        -> std::filesystem::path
    {
        auto manifest_dir = manifest_path.parent_path();
        auto exe_dir =
            Das::Utils::ToStdPath(boost::dll::program_location().parent_path());

        // Try MinGW name first, then standard name, in each directory
        for (const auto& search_dir : {manifest_dir, exe_dir})
        {
            if (auto found = TryDllInDir(search_dir, LUA_EXPORT_DLL_NAME_MINGW))
            {
                return *found;
            }
            if (auto found = TryDllInDir(search_dir, LUA_EXPORT_DLL_NAME))
            {
                return *found;
            }
        }

        // Fallback: return the standard name in manifest dir
        return manifest_dir / Das::Utils::PathFromUtf8(LUA_EXPORT_DLL_NAME);
    }
} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

LuaRuntime::LuaRuntime()
{
    lua_state_ = luaL_newstate();
    if (!lua_state_)
    {
        DAS_CORE_LOG_ERROR("Failed to create Lua state");
        return;
    }
    luaL_openlibs(lua_state_);
}

LuaRuntime::~LuaRuntime()
{
    if (lua_state_)
    {
        lua_close(lua_state_);
        lua_state_ = nullptr;
    }
}

// ============================================================================
// IDasBase
// ============================================================================

uint32_t LuaRuntime::AddRef() { return ref_counter_.AddRef(); }

uint32_t LuaRuntime::Release() { return ref_counter_.Release(this); }

DasResult LuaRuntime::QueryInterface(const DasGuid& iid, void** pp_out_object)
{
    if (pp_out_object == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    if (iid == DAS_IID_BASE)
    {
        *pp_out_object = static_cast<IDasBase*>(this);
        AddRef();
        return DAS_S_OK;
    }
    return DAS_E_NO_INTERFACE;
}

// ============================================================================
// Helpers
// ============================================================================

auto LuaRuntime::ParseEntryPoint(const std::string& entry_point)
    -> std::pair<std::string, std::string>
{
    auto dot_pos = entry_point.rfind('.');
    if (dot_pos == std::string::npos)
    {
        return {entry_point, "createInstance"};
    }
    return {entry_point.substr(0, dot_pos), entry_point.substr(dot_pos + 1)};
}

// ============================================================================
// IForeignLanguageRuntime::LoadPlugin
// ============================================================================

auto LuaRuntime::LoadPlugin(const std::filesystem::path& path)
    -> DAS::Utils::Expected<DasPtr<IDasBase>>
{
    DAS_CORE_LOG_INFO(
        "[LuaRuntime::LoadPlugin] Starting load for manifest: {}",
        Das::Utils::PathToUtf8(path));

    // ── 1. Parse manifest.json ──────────────────────────────────────────
    std::ifstream file(path);
    if (!file.is_open())
    {
        DAS_CORE_LOG_ERROR(
            "Failed to open plugin manifest: {}",
            Das::Utils::PathToUtf8(path));
        return tl::make_unexpected(DAS_E_FILE_NOT_FOUND);
    }

    std::string plugin_name;
    std::string entry_point;
    {
        const std::string manifest_content(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        auto manifest_opt = DAS::Utils::ParseYyjsonFromString(manifest_content);
        if (!manifest_opt)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to parse plugin manifest: {}",
                Das::Utils::PathToUtf8(path));
            return tl::make_unexpected(DAS_E_FAIL);
        }
        auto manifest = std::move(*manifest_opt);
        auto obj = manifest.as_object();
        if (!obj)
        {
            DAS_CORE_LOG_ERROR(
                "Plugin manifest is not a JSON object: {}",
                Das::Utils::PathToUtf8(path));
            return tl::make_unexpected(DAS_E_FAIL);
        }

        // name
        auto name_val = (*obj)[std::string_view("name")];
        auto name_str = name_val.as_string();
        if (!name_str)
        {
            DAS_CORE_LOG_ERROR(
                "Plugin manifest missing 'name' field: {}",
                Das::Utils::PathToUtf8(path));
            return tl::make_unexpected(DAS_E_FAIL);
        }
        plugin_name = std::string(*name_str);

        // entryPoint
        auto ep_val = (*obj)[std::string_view("entryPoint")];
        auto ep_str = ep_val.as_string();
        if (!ep_str)
        {
            DAS_CORE_LOG_ERROR(
                "Plugin manifest missing 'entryPoint' field: {}",
                Das::Utils::PathToUtf8(path));
            return tl::make_unexpected(DAS_E_FAIL);
        }
        entry_point = std::string(*ep_str);
    }

    DAS_CORE_LOG_INFO(
        "[LuaRuntime::LoadPlugin] Plugin: {}, entryPoint: {}",
        plugin_name,
        entry_point);

    // ── 2. Load DasCoreLuaExport.dll (cached per LuaRuntime) ────────────
    if (!export_lib_.is_loaded())
    {
        auto export_dll_path = FindLuaExportDll(path);
        DAS_CORE_LOG_INFO(
            "[LuaRuntime::LoadPlugin] Loading Lua export DLL: {}",
            Das::Utils::PathToUtf8(export_dll_path));

        // Add Lua DLL directory to Windows search path so
        // libDasCoreLuaExport.dll can find liblua.dll at runtime
#ifdef _WIN32
        {
            auto lua_dll_dir = export_dll_path.parent_path();
            if (!lua_dll_dir.empty())
            {
                AddDllDirectory(lua_dll_dir.c_str());
            }
            // Also try common MSYS2/MinGW paths
            AddDllDirectory(L"E:/msys64/clang64/bin");
        }
#endif

        boost::system::error_code ec;
        export_lib_.load(Das::Utils::ToBoostPath(export_dll_path), ec);
        if (ec)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to load Lua export library: {}, error: {}",
                Das::Utils::PathToUtf8(export_dll_path),
                ec.message());
            return tl::make_unexpected(DAS_E_INVALID_FILE);
        }

        // ── 3. Get luaopen function pointer ─────────────────────────────
        if (!export_lib_.has(LUA_OPEN_FUNCTION_NAME))
        {
            DAS_CORE_LOG_ERROR(
                "Lua export library missing '{}' function",
                LUA_OPEN_FUNCTION_NAME);
            return tl::make_unexpected(DAS_E_SYMBOL_NOT_FOUND);
        }

        auto luaopen_func =
            export_lib_.get<LuaOpenFunction>(LUA_OPEN_FUNCTION_NAME);
        if (!luaopen_func)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to get '{}' from Lua export library",
                LUA_OPEN_FUNCTION_NAME);
            return tl::make_unexpected(DAS_E_SYMBOL_NOT_FOUND);
        }

        // ── 4. Call luaopen — register all Director types ───────────────
        int luaopen_result = luaopen_func(lua_state_);
        DAS_CORE_LOG_INFO(
            "[LuaRuntime::LoadPlugin] {} returned {}",
            LUA_OPEN_FUNCTION_NAME,
            luaopen_result);

        // ── 4b. Get ExtractDasBasePointer helper ──────────────────────
        // This function lives in the same DLL as the Director classes,
        // so the static_cast<IDasBase*> is performed where the complete
        // type information is available.
        if (export_lib_.has(EXTRACT_BASE_POINTER_NAME))
        {
            extract_base_ptr_ = &export_lib_.get<ExtractBasePointerFunc>(
                EXTRACT_BASE_POINTER_NAME);
            DAS_CORE_LOG_INFO(
                "[LuaRuntime::LoadPlugin] Loaded {} helper",
                EXTRACT_BASE_POINTER_NAME);
        }
        else
        {
            DAS_CORE_LOG_WARN(
                "Lua export library missing '{}' helper function",
                EXTRACT_BASE_POINTER_NAME);
        }
    }

    // ── 5. Parse entryPoint "Module.Func" ───────────────────────────────
    auto [module_name, func_name] = ParseEntryPoint(entry_point);

    // ── 6. Add manifest directory to Lua package.path ───────────────────
    {
        auto manifest_dir = Das::Utils::PathToUtf8(path.parent_path());
        // Convert backslashes to forward slashes for Lua compatibility
        std::replace(manifest_dir.begin(), manifest_dir.end(), '\\', '/');

        sol::state_view lua(lua_state_);
        sol::table      package_table = lua["package"];
        if (package_table.valid())
        {
            const std::string current_path =
                package_table["path"].get_or(std::string(""));
            package_table["path"] = manifest_dir + "/?.lua;" + current_path;
        }
    }

    // ── 7. Execute Lua script ───────────────────────────────────────────
    //    local mod = require("ModuleName")
    //    return mod.FuncName()
    std::string lua_code = "local mod = require('" + module_name
                           + "')\n"
                             "return mod."
                           + func_name + "()";

    DAS_CORE_LOG_INFO("[LuaRuntime::LoadPlugin] Executing Lua: {}", lua_code);

    sol::state_view                      lua(lua_state_);
    const sol::protected_function_result result =
        lua.safe_script(lua_code, sol::script_pass_on_error);

    if (!result.valid())
    {
        const sol::error err = result;
        DAS_CORE_LOG_ERROR("Lua script execution failed: {}", err.what());
        return tl::make_unexpected(DAS_E_FAIL);
    }

    // ── 8. Extract IDasBase* from Lua return value ──────────────────────
    const sol::object ret_obj = result.get<sol::object>(0);
    if (ret_obj.get_type() != sol::type::userdata)
    {
        DAS_CORE_LOG_ERROR(
            "Lua script did not return a valid plugin object (expected userdata)");
        return tl::make_unexpected(DAS_E_FAIL);
    }

    // Extract IDasBase* via the helper function exported by
    // DasCoreLuaExport.dll.  The static_cast<IDasBase*> happens inside
    // the DLL where the complete Director type is defined, so it is
    // guaranteed correct regardless of DLL-boundary limitations on
    // sol2's derive/weak_derive mechanism.
    //
    // Fallback: if the helper was not loaded (old export DLL), extract
    // manually via lua_touserdata.  sol2 stores usertype objects as
    // [T* | T object]; the first sizeof(T*) bytes point to the object.
    IDasBase* base_ptr = nullptr;

    if (extract_base_ptr_)
    {
        base_ptr = extract_base_ptr_(lua_state_, result.stack_index());
    }
    else
    {
        DAS_CORE_LOG_WARN(
            "ExtractDasBasePointer not available, using inline fallback");
        void* raw_memory = lua_touserdata(lua_state_, result.stack_index());
        void* const* ptr_to_obj = static_cast<void**>(raw_memory);
        base_ptr = static_cast<IDasBase*>(*ptr_to_obj);
    }

    if (!base_ptr)
    {
        DAS_CORE_LOG_ERROR("Failed to extract IDasBase from Lua return value");
        return tl::make_unexpected(DAS_E_FAIL);
    }

    DAS_CORE_LOG_INFO(
        "[LuaRuntime::LoadPlugin] Successfully loaded Lua plugin: {}",
        plugin_name);

    // The DasPtr constructor takes the C++-side reference; the Lua-side
    // birth reference stays with the userdata and is Released by the sol2
    // garbage_collect hook.
    return DasPtr<IDasBase>(base_ptr);
}

// ============================================================================
// Factory
// ============================================================================

auto CreateForeignLanguageRuntime(const ForeignLanguageRuntimeFactoryDesc&)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>
{
    return MakeDasPtr<IForeignLanguageRuntime, LuaRuntime>();
}

DAS_NS_LUAHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_LUA

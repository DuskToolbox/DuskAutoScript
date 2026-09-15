#include "CppHost.h"
#include <boost/dll/shared_library.hpp>
#include <cstdint>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/BoostPathBridge.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/Expected.h>
#include <das/Utils/PathText.h>
#include <filesystem>
#include <fstream>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_CPPHOST_BEGIN

class CppRuntime final : public IForeignLanguageRuntime
{
    DAS::Utils::RefCounter<CppRuntime> ref_counter_{};
    boost::dll::shared_library         plugin_lib_{};

public:
    uint32_t  AddRef() final { return ref_counter_.AddRef(); }
    uint32_t  Release() final { return ref_counter_.Release(this); }
    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override
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

    auto LoadPlugin(const std::filesystem::path& path)
        -> DAS::Utils::Expected<DasPtr<IDasBase>> override
    {
        DAS_CORE_LOG_INFO(
            "[CppRuntime::LoadPlugin] Starting load for manifest: {}",
            Das::Utils::PathToUtf8(path));

        if (path.extension() != DAS_PATH_LITERAL(".json"))
        {
            DAS_CORE_LOG_ERROR(
                "CppRuntime::LoadPlugin requires a manifest .json path: {}",
                Das::Utils::PathToUtf8(path));
            return tl::make_unexpected(DAS_E_INVALID_ARGUMENT);
        }

        // 1. Parse manifest.json
        std::ifstream file(path);
        if (!file.is_open())
        {
            DAS_CORE_LOG_ERROR(
                "Failed to open plugin manifest: {}",
                Das::Utils::PathToUtf8(path));
            return tl::make_unexpected(DAS_E_FILE_NOT_FOUND);
        }

        std::string plugin_name;
        std::string plugin_extension;
        {
            const std::string manifest_content(
                (std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
            auto manifest_opt =
                Das::Utils::ParseYyjsonFromString(manifest_content);
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

            auto ext_val = (*obj)[std::string_view("pluginFilenameExtension")];
            auto ext_str = ext_val.as_string();
            if (!ext_str)
            {
                DAS_CORE_LOG_ERROR(
                    "Plugin manifest missing 'pluginFilenameExtension' field: {}",
                    Das::Utils::PathToUtf8(path));
                return tl::make_unexpected(DAS_E_FAIL);
            }
            plugin_extension = std::string(*ext_str);
        }

        // 2. Construct dll path
        auto dll_path =
            path.parent_path()
            / Das::Utils::PathFromUtf8(plugin_name + "." + plugin_extension);
        DAS_CORE_LOG_INFO(
            "[CppRuntime::LoadPlugin] Constructed dll path: {}",
            Das::Utils::PathToUtf8(dll_path));

        // 3. Load dll (existing logic)
        try
        {
            boost::system::error_code ec;
            // load_with_altered_search_path 让依赖从 dll 所在目录（插件目录）
            // 开始搜索，否则 C++ 插件的同目录依赖（如 MaaFramework/ORT 等
            // 第三方 dll）因不在进程 exe 目录或 PATH 中而加载失败
            // （ERROR_MOD_NOT_FOUND）。
            plugin_lib_.load(
                Das::Utils::ToBoostPath(dll_path),
                boost::dll::load_mode::load_with_altered_search_path,
                ec);
            if (ec)
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to load plugin library: {}, error: {}",
                    Das::Utils::PathToUtf8(dll_path),
                    ec.message());
                return tl::make_unexpected(DAS_E_INVALID_FILE);
            }

            DAS_CORE_LOG_INFO(
                "[CppRuntime::LoadPlugin] Has DasCoCreatePlugin: {}",
                plugin_lib_.has(DAS_COCREATE_PLUGIN_NAME));
            const auto p_init_function =
                plugin_lib_.get<DasCoCreatePluginFunction>(
                    DAS_COCREATE_PLUGIN_NAME);
            DAS_CORE_LOG_INFO(
                "[CppRuntime::LoadPlugin] Function pointer address: {}",
                reinterpret_cast<void*>(p_init_function));
            if (p_init_function == nullptr)
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to get export function '{}' from plugin: {}",
                    DAS_COCREATE_PLUGIN_NAME,
                    Das::Utils::PathToUtf8(dll_path));
                return tl::make_unexpected(DAS_E_SYMBOL_NOT_FOUND);
            }

            DAS_CORE_LOG_INFO(
                "[CppRuntime::LoadPlugin] Calling DasCoCreatePlugin...");
            DasPtr<IDasBase> p_plugin{};
            const auto       error_code = p_init_function(p_plugin.Put());
            DAS_CORE_LOG_INFO(
                "[CppRuntime::LoadPlugin] DasCoCreatePlugin returned: {}",
                error_code);
            if (DAS::IsOk(error_code))
            {
                DAS_CORE_LOG_INFO("[CppRuntime::LoadPlugin] Success!");
                return p_plugin;
            }
            DAS_CORE_LOG_ERROR(
                "DasCoCreatePlugin returned error: {}",
                error_code);
            return tl::make_unexpected(error_code);
        }
        catch (const boost::wrapexcept<boost::system::system_error>& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return tl::make_unexpected(DAS_E_SYMBOL_NOT_FOUND);
        }
        catch (const std::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return tl::make_unexpected(DAS_E_INTERNAL_FATAL_ERROR);
        }
    }
};

auto CreateForeignLanguageRuntime(const ForeignLanguageRuntimeFactoryDesc&)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>
{
    return MakeDasPtr<IForeignLanguageRuntime, CppRuntime>();
}

DAS_NS_CPPHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

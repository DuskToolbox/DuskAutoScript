#include "CppHost.h"
#include <boost/dll/shared_library.hpp>
#include <cstdint>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

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
            path.string());

        // 1. Parse manifest.json
        std::ifstream file(path);
        if (!file.is_open())
        {
            DAS_CORE_LOG_ERROR(
                "Failed to open plugin manifest: {}",
                path.string());
            return tl::make_unexpected(DAS_E_FILE_NOT_FOUND);
        }

        std::string plugin_name;
        std::string plugin_extension;
        try
        {
            nlohmann::json manifest = nlohmann::json::parse(file);

            if (!manifest.contains("name"))
            {
                DAS_CORE_LOG_ERROR(
                    "Plugin manifest missing 'name' field: {}",
                    path.string());
                return tl::make_unexpected(DAS_E_FAIL);
            }
            if (!manifest.contains("pluginFilenameExtension"))
            {
                DAS_CORE_LOG_ERROR(
                    "Plugin manifest missing 'pluginFilenameExtension' field: {}",
                    path.string());
                return tl::make_unexpected(DAS_E_FAIL);
            }

            manifest["name"].get_to(plugin_name);
            manifest["pluginFilenameExtension"].get_to(plugin_extension);
        }
        catch (const nlohmann::json::exception& e)
        {
            DAS_CORE_LOG_ERROR("Failed to parse plugin manifest: {}", e.what());
            return tl::make_unexpected(DAS_E_FAIL);
        }

        // 2. Construct dll path
        auto dll_path =
            path.parent_path() / (plugin_name + "." + plugin_extension);
        DAS_CORE_LOG_INFO(
            "[CppRuntime::LoadPlugin] Constructed dll path: {}",
            dll_path.string());

        // 3. Load dll (existing logic)
        try
        {
            boost::system::error_code ec;
            plugin_lib_.load(dll_path.wstring(), ec);
            if (ec)
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to load plugin library: {}, error: {}",
                    dll_path.string(),
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
                    dll_path.string());
                return tl::make_unexpected(DAS_E_SYMBOL_NOT_FOUND);
            }

            DAS_CORE_LOG_INFO(
                "[CppRuntime::LoadPlugin] Calling DasCoCreatePlugin...");
            DasPtr<IDasBase> p_plugin{};
            const auto       error_code = p_init_function(p_plugin.Put());
            DAS_CORE_LOG_INFO(
                "[CppRuntime::LoadPlugin] DasCoCreatePlugin returned: {:#x}",
                static_cast<uint32_t>(error_code));
            if (DAS::IsOk(error_code))
            {
                DAS_CORE_LOG_INFO("[CppRuntime::LoadPlugin] Success!");
                return p_plugin;
            }
            DAS_CORE_LOG_ERROR(
                "DasCoCreatePlugin returned error: {:#x}",
                static_cast<uint32_t>(error_code));
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

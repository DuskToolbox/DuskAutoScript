#include "CppHost.h"
#include <boost/dll/shared_library.hpp>
#include <cstdint>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_CPPHOST_BEGIN

using DasCoCreatePluginFunction = DasResult (*)(IDasBase**);

constexpr const char* DAS_COCREATE_PLUGIN_NAME = "DasCoCreatePlugin";

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
        try
        {
            boost::system::error_code ec;
            plugin_lib_.load(path.wstring(), ec);
            if (ec)
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to load plugin library: {}, error: {}",
                    path.string(),
                    ec.message());
                return tl::make_unexpected(DAS_E_INVALID_FILE);
            }

            const auto p_init_function =
                plugin_lib_.get<DasCoCreatePluginFunction>(
                    DAS_COCREATE_PLUGIN_NAME);
            if (p_init_function == nullptr)
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to get export function '{}' from plugin: {}",
                    DAS_COCREATE_PLUGIN_NAME,
                    path.string());
                return tl::make_unexpected(DAS_E_SYMBOL_NOT_FOUND);
            }

            DasPtr<IDasBase> p_plugin{};
            const auto       error_code = p_init_function(p_plugin.Put());
            if (DAS::IsOk(error_code))
            {
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

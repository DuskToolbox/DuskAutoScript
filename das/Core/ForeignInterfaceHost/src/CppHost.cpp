#include "CppHost.h"
#include <boost/dll/shared_library.hpp>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/PluginInterface/IDasPluginPackage.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_CPPHOST_BEGIN

class CppRuntime final : public IForeignLanguageRuntime
{
    DAS::Utils::RefCounter<CppRuntime> ref_counter_{};
    boost::dll::shared_library         plugin_lib_{};

public:
    int64_t   AddRef() override { return ref_counter_.AddRef(); }
    int64_t   Release() override { return ref_counter_.Release(this); }
    DasResult QueryInterface(const DasGuid&, void**) override
    {
        return DAS_E_NO_IMPLEMENTATION;
    }
    auto LoadPlugin(const std::filesystem::path& path)
        -> DAS::Utils::Expected<CommonPluginPtr> override
    {
        try
        {
            plugin_lib_.load(path.c_str());
            // Get function pointer without heap allocation.
            const auto& p_init_function =
                plugin_lib_.get<::DasCoCreatePluginFunction>(
                    DASCOCREATEPLUGIN_NAME);
            DasPtr<IDasPluginPackage> p_plugin{};
            const auto error_code = p_init_function(p_plugin.Put());
            if (DAS::IsOk(error_code))
            {
                return p_plugin;
            }
            return tl::make_unexpected(error_code);
        }
        catch (const boost::wrapexcept<boost::system::system_error>& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return tl::make_unexpected(DAS_E_SYMBOL_NOT_FOUND);
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

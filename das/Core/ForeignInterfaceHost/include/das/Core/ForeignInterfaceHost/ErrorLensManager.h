#ifndef DAS_CORE_FOREIGNINTERFACEHOST_ERRORLENSMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_ERRORLENSMANAGER_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/DasString.hpp>
#include <das/Utils/Expected.h>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

struct FeatureInfo;

class ErrorLensManager
{
private:
    struct Route
    {
        DasGuid                                plugin_guid{};
        DasPtr<PluginInterface::IDasErrorLens> lens{};
    };

    std::unordered_map<DasGuid, Route>                routes_{};
    std::unordered_map<DasGuid, std::vector<DasGuid>> plugin_routes_{};
    mutable std::shared_mutex                         mutex_;

    DasResult RegisterRoutes(
        const DasGuid*                  p_plugin_guid,
        std::span<const DasGuid>        guids,
        PluginInterface::IDasErrorLens* p_error_lens);

public:
    DasResult OnPluginLoaded(
        const DasGuid&                plugin_guid,
        std::span<FeatureInfo* const> error_lens_features);

    DasResult OnPluginUnloading(const DasGuid& plugin_guid);

    DasResult Register(
        Das::ExportInterface::IDasReadOnlyGuidVector* p_guid_vector,
        PluginInterface::IDasErrorLens*               p_error_lens);

    DasResult FindInterface(
        const DasGuid&                   iid,
        PluginInterface::IDasErrorLens** pp_out_lens);

    auto GetErrorMessage(
        const DasGuid&      iid,
        IDasReadOnlyString* locale_name,
        DasResult           error_code) const
        -> DAS::Utils::Expected<DasPtr<IDasReadOnlyString>>;
};

void              SetActiveErrorLensManager(ErrorLensManager* p_manager);
void              ClearActiveErrorLensManager(ErrorLensManager* p_manager);
ErrorLensManager* GetActiveErrorLensManager();

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_ERRORLENSMANAGER_H

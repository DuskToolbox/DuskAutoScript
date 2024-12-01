#ifndef DAS_HTTP_CONTROLLER_DASINITIALIZEPLUGINMANAGERCALLBACK_H
#define DAS_HTTP_CONTROLLER_DASINITIALIZEPLUGINMANAGERCALLBACK_H

#include "das/ExportInterface/IDasPluginManager.h"
#include "das/Utils/CommonUtils.hpp"

class DasInitializePluginManagerCallback
    : public IDasInitializeIDasPluginManagerCallback
{
    DAS_UTILS_IDASBASE_AUTO_IMPL(DasInitializePluginManagerCallback)
    DasResult initialize_result_{DAS_E_UNDEFINED_RETURN_VALUE};
    DAS::DasPtr<IDasPluginManagerForUi> plugin_manager_for_ui_;

    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;
    DasResult OnFinished(
        DasResult               initialize_result,
        IDasPluginManagerForUi* p_in_manager_without_owner_ship) override;

public:
    DasResult                           GetInitializeResult();
    DAS::DasPtr<IDasPluginManagerForUi> GetPluginManagerForUi();
};

#endif // DAS_HTTP_CONTROLLER_DASINITIALIZEPLUGINMANAGERCALLBACK_H

#include "DasInitializePluginManagerCallback.h"
#include "das/Utils/QueryInterface.hpp"

DasResult DasInitializePluginManagerCallback::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    return DAS::Utils::QueryInterface<IDasInitializeIDasPluginManagerCallback>(
        this,
        iid,
        pp_object);
}

DasResult DasInitializePluginManagerCallback::OnFinished(
    DasResult               initialize_result,
    IDasPluginManagerForUi* p_in_manager_without_owner_ship)
{
    initialize_result_ = initialize_result;
    plugin_manager_for_ui_ = p_in_manager_without_owner_ship;
}

DasResult DasInitializePluginManagerCallback::GetInitializeResult()
{
    return initialize_result_;
}

Das::DasPtr<IDasPluginManagerForUi>
DasInitializePluginManagerCallback::GetPluginManagerForUi()
{
    return plugin_manager_for_ui_;
}
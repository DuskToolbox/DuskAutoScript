#include <das/Core/IPC/MainProcessServer.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/DasApi.h>
#include <das/IDasBase.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        DasResult IpcLoadPluginImpl(
            const std::string& plugin_path,
            IDasBase**         pp_out_plugin)
        {
            if (pp_out_plugin == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }

            *pp_out_plugin = nullptr;

            auto& server = MainProcessServer::GetInstance();

            RemoteObjectInfo object_info;
            DasResult result = server.SendLoadPlugin(plugin_path, object_info);

            if (result != DAS_S_OK)
            {
                return result;
            }

            auto& factory = ProxyFactory::GetInstance();

            if (!factory.IsInitialized())
            {
                return DAS_E_IPC_INVALID_STATE;
            }

            DasPtr<IDasBase> proxy =
                factory.CreateProxy<IDasBase>(object_info.object_id);

            if (!proxy)
            {
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            *pp_out_plugin = proxy.Get();
            proxy->AddRef();

            return DAS_S_OK;
        }
    }
}
DAS_NS_END

DAS_C_API DasResult
IpcLoadPlugin(const char* p_plugin_path, IDasBase** pp_out_plugin)
{
    if (p_plugin_path == nullptr || pp_out_plugin == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    return DAS::Core::IPC::IpcLoadPluginImpl(
        std::string(p_plugin_path),
        pp_out_plugin);
}

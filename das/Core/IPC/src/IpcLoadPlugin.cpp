#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/MainProcess/MainProcessServer.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <stdexec/execution.hpp>
DAS_CORE_IPC_NS_BEGIN
DasResult IpcLoadPluginImpl(
    const std::string& plugin_path,
    IDasBase**         pp_out_plugin)
{
    if (pp_out_plugin == nullptr)
    {
        DAS_CORE_LOG_ERROR("pp_out_plugin is null");
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_plugin = nullptr;

    auto& server = MainProcess::MainProcessServer::GetInstance();

    // 获取第一个可用的 session_id
    // TODO: 需要调用者显式指定 session_id
    auto& conn_manager = ConnectionManager::GetInstance();
    auto  sessions = conn_manager.GetConnectedSessions();

    if (sessions.empty())
    {
        DAS_CORE_LOG_ERROR("No available connections");
        return DAS_E_IPC_NO_CONNECTIONS;
    }
    uint16_t session_id = sessions[0];

    ObjectId                             object_id{};
    DasPtr<IDasAsyncLoadPluginOperation> op;
    DasResult                            result =
        server.SendLoadPluginAsync(plugin_path, session_id, op.Put());

    if (result != DAS_S_OK)
    {
        return result;
    }

    // 等待异步操作完成
    auto await_result = stdexec::sync_wait(AwaitAsync(std::move(op)));
    if (!await_result)
    {
        DAS_CORE_LOG_ERROR("sync_wait failed");
        return DAS_E_IPC_TIMEOUT;
    }

    auto [load_result, loaded_object_id] = *await_result;
    if (load_result != DAS_S_OK)
    {
        return load_result;
    }
    object_id = loaded_object_id;

    auto& factory = ProxyFactory::GetInstance();

    if (!factory.IsInitialized())
    {
        DAS_CORE_LOG_ERROR("ProxyFactory not initialized");
        return DAS_E_IPC_INVALID_STATE;
    }
    DasPtr<IDasBase> proxy = factory.CreateProxy<IDasBase>(object_id);

    if (!proxy)
    {
        DAS_CORE_LOG_ERROR("Failed to create proxy");
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }
    *pp_out_plugin = proxy.Get();
    proxy->AddRef();

    return DAS_S_OK;
}
DAS_CORE_IPC_NS_END

DAS_C_API DasResult
IpcLoadPlugin(const char* p_plugin_path, IDasBase** pp_out_plugin)
{
    if (p_plugin_path == nullptr || pp_out_plugin == nullptr)
    {
        DAS_CORE_LOG_ERROR("Invalid pointer argument");
        return DAS_E_INVALID_POINTER;
    }
    return DAS::Core::IPC::IpcLoadPluginImpl(
        std::string(p_plugin_path),
        pp_out_plugin);
}

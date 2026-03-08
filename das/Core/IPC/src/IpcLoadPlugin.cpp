#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <atomic>
#include <stdexec/execution.hpp>

#ifndef DAS_E_NOT_IMPLEMENTED
#define DAS_E_NOT_IMPLEMENTED DAS_E_NO_IMPLEMENTATION
#endif

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief 简单的 IHostLauncher 包装器，仅包装 session_id
 *
 * 用于 IpcLoadPlugin C API，该 API 没有真正的 HostLauncher 实例。
 * 这个包装器仅提供 GetSessionId() 实现。
 */
class SimpleHostLauncherWrapper : public IHostLauncher
{
public:
    explicit SimpleHostLauncherWrapper(uint16_t session_id)
        : session_id_(session_id), ref_(1)
    {
    }

    // IHostLauncher 接口
    DasResult StartAsync(
        const std::string& /*host_exe_path*/,
        IDasAsyncHandshakeOperation** /*pp_out_operation*/) override
    {
        DAS_CORE_LOG_ERROR("SimpleHostLauncherWrapper::StartAsync not supported");
        return DAS_E_NOT_IMPLEMENTED;
    }

    DasResult Start(
        const std::string& /*host_exe_path*/,
        uint16_t& /*out_session_id*/,
        uint32_t /*timeout_ms*/) override
    {
        DAS_CORE_LOG_ERROR("SimpleHostLauncherWrapper::Start not supported");
        return DAS_E_NOT_IMPLEMENTED;
    }

    void Stop() override
    {
        // No-op
    }

    [[nodiscard]] bool IsRunning() const override { return session_id_ != 0; }

    [[nodiscard]] uint32_t GetPid() const override { return 0; }

    [[nodiscard]] uint16_t GetSessionId() const override { return session_id_; }

    // IDasBase 接口
    uint32_t AddRef() override { return ++ref_; }

    uint32_t Release() override
    {
        auto r = --ref_;
        if (r == 0)
            delete this;
        return r;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp) override
    {
        if (iid == DasIidOf<IDasBase>())
        {
            AddRef();
            *pp = static_cast<IDasBase*>(this);
            return DAS_S_OK;
        }
        return DAS_E_NO_INTERFACE;
    }

private:
    uint16_t               session_id_;
    std::atomic<uint32_t>  ref_;
};

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

    // 创建 IPC 上下文
    auto ctx = MainProcess::CreateIpcContextEz();
    if (!ctx)
    {
        DAS_CORE_LOG_ERROR("Failed to create IpcContext");
        return DAS_E_FAIL;
    }

    // 获取第一个可用的 session_id
    auto sessions = ctx->GetConnectedSessions();

    if (sessions.empty())
    {
        DAS_CORE_LOG_ERROR("No available connections");
        return DAS_E_IPC_NO_CONNECTIONS;
    }
    uint16_t session_id = sessions[0];

    // 创建简单的 IHostLauncher 包装器
    auto* launcher = new SimpleHostLauncherWrapper(session_id);
    DasPtr<IHostLauncher> launcher_ptr(launcher);

    ObjectId                             object_id{};
    DasPtr<IDasAsyncLoadPluginOperation> op;
    DasResult                            result = ctx->LoadPluginAsync(
        launcher_ptr.Get(), plugin_path.c_str(), op.Put());

    if (result != DAS_S_OK)
    {
        return result;
    }

    // 使用 IPC 感知的 wait 驱动消息循环
    auto await_result = wait(*ctx, async_op(*ctx, std::move(op)));
    if (!await_result)
    {
        DAS_CORE_LOG_ERROR("wait failed");
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

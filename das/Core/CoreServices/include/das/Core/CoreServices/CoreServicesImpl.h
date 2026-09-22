#ifndef DAS_CORE_CORE_SERVICES_CORESERVICESIMPL_H
#define DAS_CORE_CORE_SERVICES_CORESERVICESIMPL_H

#include <atomic>
#include <das/Core/CoreServices/Config.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/DasPtr.hpp>
#include <das/DasRef.hpp>
#include <das/IDasCoreServices.h>
#include <memory>

DAS_CORE_CORE_SERVICES_NS_BEGIN

/**
 * @brief IDasCoreServices 实现
 *
 * 拥有 DasCore 业务服务的完整构造顺序：
 * SettingsManager -> PluginManager -> SchedulerService
 * 然后为每个服务创建 COM 风格的包装器。
 *
 * 接收已初始化的 IIpcContext（不负责创建/运行/关闭），
 * 以 DasRef 引用计数共享生命周期。
 */
class CoreServicesImpl final : public IDasCoreServices
{
public:
    explicit CoreServicesImpl(
        DasRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context,
        std::filesystem::path                            settings_dir,
        std::filesystem::path                            plugin_dir);

    ~CoreServicesImpl();

    // IDasBase
    uint32_t DAS_STD_CALL AddRef() override;
    uint32_t DAS_STD_CALL Release() override;
    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out) override;

    // IDasCoreServices
    DAS_IMPL GetSettingsService(IDasSettingsService** pp_out) override;
    DAS_IMPL GetPluginManagerService(
        IDasPluginManagerService** pp_out) override;
    DAS_IMPL GetSchedulerService(IDasSchedulerService** pp_out) override;
    DAS_IMPL Shutdown() override;

    /// 借用内部拥有的 IPC context（裸指针，生命周期随本对象）
    DAS::Core::IPC::MainProcess::IIpcContext* GetIpcContext() const;

    /// 借用内部拥有的具体调度器服务（引用，生命周期随本对象）。主进程
    /// HTTP 直连装配用：DasHttp 控制器进程内直连具体类，不经调度器接口
    /// 面（该接口已定退役）。跨 DLL 消费经 BorrowSchedulerService 导出
    /// 入口（CoreServicesBorrow.h），不直接暴露本类。
    Das::Core::TaskScheduler::SchedulerService& SchedulerServiceInstance();

private:
    DasResult ShutdownCoreRuntime() noexcept;

    std::atomic<uint32_t> ref_count_{0};
    std::atomic_bool      shutdown_called_{false};

    // IPC context (shared ownership via refcount)
    DasRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context_;

    // Concrete business services (owned)
    Das::Core::SettingsManager::SettingsManager    settings_manager_;
    Das::Core::ForeignInterfaceHost::PluginManager plugin_manager_;
    Das::Core::TaskScheduler::SchedulerService     scheduler_svc_;

    // COM-style service wrappers (owned)
    DasPtr<IDasSettingsService>      settings_service_;
    DasPtr<IDasPluginManagerService> plugin_manager_service_;
    DasPtr<IDasSchedulerService>     scheduler_service_;
};

DAS_CORE_CORE_SERVICES_NS_END

#endif // DAS_CORE_CORE_SERVICES_CORESERVICESIMPL_H

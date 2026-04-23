#ifndef DAS_CORE_CORE_SERVICES_CORESERVICESIMPL_H
#define DAS_CORE_CORE_SERVICES_CORESERVICESIMPL_H

#include <atomic>
#include <das/Core/CoreServices/Config.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/DasPtr.hpp>
#include <das/DasSharedRef.hpp>
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
 * 内部包装为 DasSharedRef<IIpcContext> 保持 RAII 生命周期。
 */
class CoreServicesImpl final : public IDasCoreServices
{
public:
    explicit CoreServicesImpl(
        DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context,
        std::filesystem::path                                  settings_dir,
        std::filesystem::path                                  plugin_dir);

    ~CoreServicesImpl() = default;

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

private:
    std::atomic<uint32_t> ref_count_{0};

    // IPC context (shared ownership via DasSharedRef)
    DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context_;

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

#include <das/Core/CoreServices/CoreServicesImpl.h>
#include <das/Core/ForeignInterfaceHost/PluginManagerServiceImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/SettingsManager/SettingsServiceImpl.h>
#include <das/Core/TaskScheduler/SchedulerServiceImpl.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <new>

DAS_CORE_CORE_SERVICES_NS_BEGIN

CoreServicesImpl::CoreServicesImpl(
    DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context,
    std::filesystem::path                                  settings_dir,
    std::filesystem::path                                  plugin_dir)
    : ipc_context_{std::move(ipc_context)},
      settings_manager_{std::move(settings_dir)},
      plugin_manager_{settings_manager_}, scheduler_service_{plugin_manager_}
{
    // 将 IPC context 注入到需要的具体服务中
    plugin_manager_.SetIpcContext(ipc_context_.get());
    scheduler_service_.SetIpcContext(ipc_context_.get());

    // 创建 COM 风格的服务包装器
    auto* settings_impl =
        new Das::Core::SettingsManager::SettingsServiceImpl(settings_manager_);
    settings_impl->AddRef();
    settings_service_ = DasPtr<IDasSettingsService>::Attach(settings_impl);

    auto* plugin_mgr_impl =
        new Das::Core::ForeignInterfaceHost::PluginManagerServiceImpl(
            plugin_manager_);
    plugin_mgr_impl->AddRef();
    plugin_manager_service_ =
        DasPtr<IDasPluginManagerService>::Attach(plugin_mgr_impl);

    auto* scheduler_impl =
        new Das::Core::TaskScheduler::SchedulerServiceImpl(scheduler_service_);
    scheduler_impl->AddRef();
    scheduler_service_ = DasPtr<IDasSchedulerService>::Attach(scheduler_impl);
}

uint32_t DAS_STD_CALL CoreServicesImpl::AddRef() { return ++ref_count_; }

uint32_t DAS_STD_CALL CoreServicesImpl::Release()
{
    auto count = --ref_count_;
    if (count == 0)
    {
        delete this;
    }
    return count;
}

DasResult DAS_STD_CALL
CoreServicesImpl::QueryInterface(const DasGuid& iid, void** pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    if (iid == DasIidOf<IDasBase>())
    {
        *pp_out = static_cast<IDasBase*>(this);
        AddRef();
        return DAS_S_OK;
    }

    if (iid == DasIidOf<IDasCoreServices>())
    {
        *pp_out = static_cast<IDasCoreServices*>(this);
        AddRef();
        return DAS_S_OK;
    }

    *pp_out = nullptr;
    return DAS_E_NO_INTERFACE;
}

DasResult CoreServicesImpl::GetSettingsService(IDasSettingsService** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    if (!settings_service_)
    {
        return DAS_E_NOT_FOUND;
    }
    *pp_out = settings_service_.Get();
    settings_service_->AddRef();
    return DAS_S_OK;
}

DasResult CoreServicesImpl::GetPluginManagerService(
    IDasPluginManagerService** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    if (!plugin_manager_service_)
    {
        return DAS_E_NOT_FOUND;
    }
    *pp_out = plugin_manager_service_.Get();
    plugin_manager_service_->AddRef();
    return DAS_S_OK;
}

DasResult CoreServicesImpl::GetSchedulerService(IDasSchedulerService** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    if (!scheduler_service_)
    {
        return DAS_E_NOT_FOUND;
    }
    *pp_out = scheduler_service_.Get();
    scheduler_service_->AddRef();
    return DAS_S_OK;
}

DAS_CORE_CORE_SERVICES_NS_END

DAS_C_API DasResult CreateIDasCoreServices(
    IDasReadOnlyString*                       p_settings_dir,
    IDasReadOnlyString*                       p_plugin_dir,
    Das::Core::IPC::MainProcess::IIpcContext* p_ipc_context,
    IDasCoreServices**                        pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (p_settings_dir == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (p_plugin_dir == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (p_ipc_context == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        // 反序列化路径参数
        const char* u8_settings_dir = nullptr;
        auto        result = p_settings_dir->GetUtf8(&u8_settings_dir);
        if (Das::IsFailed(result))
        {
            return result;
        }
        std::filesystem::path settings_dir = std::filesystem::path(
            reinterpret_cast<const char8_t*>(u8_settings_dir));

        const char* u8_plugin_dir = nullptr;
        result = p_plugin_dir->GetUtf8(&u8_plugin_dir);
        if (Das::IsFailed(result))
        {
            return result;
        }
        std::filesystem::path plugin_dir = std::filesystem::path(
            reinterpret_cast<const char8_t*>(u8_plugin_dir));

        // 包装 IPC context 为 DasSharedRef（获取所有权）
        Das::DasSharedRef<Das::Core::IPC::MainProcess::IIpcContext> ipc_context{
            p_ipc_context,
            Das::Core::IPC::MainProcess::IpcContextDeleter{}};

        auto* impl = new Das::Core::CoreServices::CoreServicesImpl(
            std::move(ipc_context),
            std::move(settings_dir),
            std::move(plugin_dir));
        impl->AddRef();
        *pp_out = impl;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (const std::invalid_argument& ex)
    {
        DAS_CORE_LOG_ERROR("CreateIDasCoreServices: {}", ex.what());
        return DAS_E_INVALID_ARGUMENT;
    }
}

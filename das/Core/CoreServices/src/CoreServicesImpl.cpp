#include <das/Core/CoreServices/CoreServicesImpl.h>
#include <das/Core/ForeignInterfaceHost/PluginManagerServiceImpl.h>
#include <das/Core/ForeignInterfaceHost/PluginResourceIndex.h>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
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
      plugin_manager_{
          settings_manager_,
          DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
              ipc_context_.shared())},
      scheduler_svc_{
          plugin_manager_,
          DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(
              ipc_context_.shared())}
{
    Das::Core::ForeignInterfaceHost::CleanupMarkedPlugins(plugin_dir);

    Das::Core::ForeignInterfaceHost::PluginResourceIndex::GetInstance()
        .ConfigurePluginResourceScanRoot(plugin_dir);

    // 创建 COM 风格的服务包装器
    auto* settings_impl =
        new Das::Core::SettingsManager::SettingsServiceImpl(settings_manager_);
    settings_impl->AddRef();
    settings_service_ = DasPtr<IDasSettingsService>::Attach(settings_impl);

    auto* plugin_mgr_impl =
        new Das::Core::ForeignInterfaceHost::PluginManagerServiceImpl(
            plugin_manager_,
            plugin_dir);
    plugin_mgr_impl->AddRef();
    plugin_manager_service_ =
        DasPtr<IDasPluginManagerService>::Attach(plugin_mgr_impl);

    auto* scheduler_impl =
        new Das::Core::TaskScheduler::SchedulerServiceImpl(scheduler_svc_);
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
    DasOutPtr<IDasSettingsService> result(pp_out);
    if (!settings_service_)
    {
        return DAS_E_NOT_FOUND;
    }
    result.Set(settings_service_.Get());
    result.Keep();
    return DAS_S_OK;
}

DasResult CoreServicesImpl::GetPluginManagerService(
    IDasPluginManagerService** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    DasOutPtr<IDasPluginManagerService> result(pp_out);
    if (!plugin_manager_service_)
    {
        return DAS_E_NOT_FOUND;
    }
    result.Set(plugin_manager_service_.Get());
    result.Keep();
    return DAS_S_OK;
}

DasResult CoreServicesImpl::GetSchedulerService(IDasSchedulerService** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    DasOutPtr<IDasSchedulerService> result(pp_out);
    if (!scheduler_service_)
    {
        return DAS_E_NOT_FOUND;
    }
    result.Set(scheduler_service_.Get());
    result.Keep();
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
    Das::DasOutPtr<IDasCoreServices> out(pp_out);
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
        // Take ownership before any fallible path conversion below so the
        // raw IPC context is released even when argument decoding fails.
        Das::DasSharedRef<Das::Core::IPC::MainProcess::IIpcContext> ipc_context{
            p_ipc_context,
            Das::Core::IPC::MainProcess::IpcContextDeleter{}};

        // 反序列化路径参数
        const char* u8_settings_dir = nullptr;
        auto        get_result = p_settings_dir->GetUtf8(&u8_settings_dir);
        if (Das::IsFailed(get_result))
        {
            return get_result;
        }
        std::filesystem::path settings_dir = std::filesystem::path(
            reinterpret_cast<const char8_t*>(u8_settings_dir));

        const char* u8_plugin_dir = nullptr;
        get_result = p_plugin_dir->GetUtf8(&u8_plugin_dir);
        if (Das::IsFailed(get_result))
        {
            return get_result;
        }
        std::filesystem::path plugin_dir = std::filesystem::path(
            reinterpret_cast<const char8_t*>(u8_plugin_dir));

        auto* impl = new Das::Core::CoreServices::CoreServicesImpl(
            std::move(ipc_context),
            std::move(settings_dir),
            std::move(plugin_dir));
        impl->AddRef();
        *out.Put() = impl;
        out.Keep();
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

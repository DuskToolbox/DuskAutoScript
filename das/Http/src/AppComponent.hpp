#ifndef DAS_HTTP_APP_COMPONENT_HPP
#define DAS_HTTP_APP_COMPONENT_HPP

#include "beast/Router.hpp"
#include "beast/Server.hpp"
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasCoreServices.h>
#include <das/IDasPluginManagerService.h>
#include <das/IDasSchedulerService.h>
#include <das/IDasSettingsService.h>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace Das::Http
{
    class AppComponent
    {
    public:
        std::shared_ptr<Beast::Router> router;
        std::function<bool()>          stop_condition;

        // CoreServices bundle (owns concrete service construction internally)
        DasPtr<IDasCoreServices> core_services;

        // Service interfaces obtained through IDasCoreServices
        DasPtr<IDasSettingsService>      settings_service;
        DasPtr<IDasPluginManagerService> plugin_mgr_service;
        DasPtr<IDasSchedulerService>     scheduler_svc;

        std::filesystem::path plugin_dir;

        // IPC context (process-level)
        std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context;

        std::thread ipc_thread;

        explicit AppComponent(const std::filesystem::path& plugin_dir)
            : router(std::make_shared<Beast::Router>()), plugin_dir(plugin_dir)
        {
        }
    };

} // namespace Das::Http

#endif // DAS_HTTP_APP_COMPONENT_HPP

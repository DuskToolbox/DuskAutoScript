#ifndef DAS_HTTP_APP_COMPONENT_HPP
#define DAS_HTTP_APP_COMPONENT_HPP

#include "beast/Router.hpp"
#include "beast/Server.hpp"
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasPluginManagerService.h>
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

        // Concrete instances (construction order matters — must be declared
        // before interface wrappers)
        Das::Core::SettingsManager::SettingsManager    settings_manager;
        Das::Core::ForeignInterfaceHost::PluginManager plugin_manager;
        Das::Core::TaskScheduler::SchedulerService     scheduler_service;
        std::filesystem::path                          plugin_dir;

        // Interface pointers via Create factory (per D-06)
        DasPtr<IDasSettingsService>      settings_service;
        DasPtr<IDasPluginManagerService> plugin_mgr_service;

        // IPC context (process-level)
        std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context;

        std::thread ipc_thread;

        explicit AppComponent(const std::filesystem::path& plugin_dir)
            : router(std::make_shared<Beast::Router>()),
              settings_manager(std::filesystem::path("settings")),
              plugin_manager(settings_manager),
              scheduler_service(plugin_manager), plugin_dir(plugin_dir)
        {
            // Create interface wrappers per D-06
            CreateDasSettingsService(settings_manager, settings_service.Put());
            CreateDasPluginManagerService(
                plugin_manager,
                plugin_mgr_service.Put());
        }
    };

} // namespace Das::Http

#endif // DAS_HTTP_APP_COMPONENT_HPP

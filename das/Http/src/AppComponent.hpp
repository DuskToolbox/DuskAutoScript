#ifndef DAS_HTTP_APP_COMPONENT_HPP
#define DAS_HTTP_APP_COMPONENT_HPP

#include "NotificationHub.hpp"
#include "beast/Router.hpp"
#include "beast/Server.hpp"
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

namespace Das::Core::TaskScheduler
{
    class SchedulerService;
}

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

        // 进程内直连的具体调度器（借用指针，生命周期随 core_services
        // 引用——BorrowSchedulerService 出借，控制器直连消费）
        Das::Core::TaskScheduler::SchedulerService* scheduler_service = nullptr;

        std::filesystem::path plugin_dir;

        std::thread ipc_thread;

        // WebSocket notification hub
        std::shared_ptr<NotificationHub> notification_hub;

        explicit AppComponent(const std::filesystem::path& plugin_dir)
            : router(std::make_shared<Beast::Router>()), plugin_dir(plugin_dir)
        {
        }
    };

} // namespace Das::Http

#endif // DAS_HTTP_APP_COMPONENT_HPP

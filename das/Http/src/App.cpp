#include <iostream>
#include <ostream>

#include "das/Utils/ThreadUtils.h"

#include "./AppComponent.hpp"
#include "./controller/DasLogController.hpp"
#include "./controller/DasMiscController.hpp"
#include "./controller/DasPluginManagerController.hpp"
#include "./controller/DasProfileController.hpp"
#include "./controller/UISettingsController.hpp"
#include "./beast/Server.hpp"

namespace Das::Http
{
    DAS_DEFINE_VARIABLE(g_server_condition){};

    void ServerCondition::RequestServerStop()
    {
        server_should_continue_ = false;
    }

    std::function<bool()> ServerCondition::GetCondition()
    {
        return [this]() -> bool { return server_should_continue_; };
    }
}

DasResult run()
{
    AppComponent components;

    const auto init_result = InitializeDasCore();
    if (DAS::IsFailed(init_result))
    {
        std::cerr << "Init DAS Core failed. Error code = " << init_result << std::endl;
        return init_result;
    }

    // 创建控制器实例
    auto misc_controller = std::make_shared<DasMiscController>();
    auto log_controller = std::make_shared<DasLogController>();
    auto profile_controller = std::make_shared<DasProfileManagerController>();
    auto plugin_controller = std::make_shared<DasPluginManagerController>();
    auto settings_controller = std::make_shared<DasUiSettingsController>();

    // 注册路由
    // Misc
    components.router->Post(DAS_HTTP_API_PREFIX "alive", 
        [misc_controller](const Beast::HttpRequest& req) { return misc_controller->Alive(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "request_shutdown",
        [misc_controller](const Beast::HttpRequest& req) { return misc_controller->RequestShutdown(req); });

    // Log
    components.router->Post(DAS_HTTP_API_PREFIX "logs",
        [log_controller](const Beast::HttpRequest& req) { return log_controller->GetLogs(req); });

    // Profile
    components.router->Post(DAS_HTTP_API_PREFIX "profile/list",
        [profile_controller](const Beast::HttpRequest& req) { return profile_controller->GetProfileList(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "profile/get",
        [profile_controller](const Beast::HttpRequest& req) { return profile_controller->GetProfile(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "profile/status",
        [profile_controller](const Beast::HttpRequest& req) { return profile_controller->GetProfileStatus(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "profile/create",
        [profile_controller](const Beast::HttpRequest& req) { return profile_controller->CreateProfile(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "profile/delete",
        [profile_controller](const Beast::HttpRequest& req) { return profile_controller->DeleteProfile(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "profile/save",
        [profile_controller](const Beast::HttpRequest& req) { return profile_controller->SaveProfile(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "profile/enable",
        [profile_controller](const Beast::HttpRequest& req) { return profile_controller->SetEnable(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "profile/start",
        [profile_controller](const Beast::HttpRequest& req) { return profile_controller->StartProfile(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "profile/stop",
        [profile_controller](const Beast::HttpRequest& req) { return profile_controller->StopProfile(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "settings/task/list",
        [profile_controller](const Beast::HttpRequest& req) { return profile_controller->GetTaskList(req); });

    // Plugin Manager
    components.router->Post(DAS_HTTP_API_PREFIX "profile/initialize",
        [plugin_controller](const Beast::HttpRequest& req) { return plugin_controller->Initialize(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "settings/plugin/list",
        [plugin_controller](const Beast::HttpRequest& req) { return plugin_controller->GetPluginList(req); });

    // Settings
    components.router->Post(DAS_HTTP_API_PREFIX "settings/get",
        [settings_controller](const Beast::HttpRequest& req) { return settings_controller->V1SettingsGet(req); });
    components.router->Post(DAS_HTTP_API_PREFIX "settings/update",
        [settings_controller](const Beast::HttpRequest& req) { return settings_controller->V1SettingsUpdate(req); });

    // 创建并启动服务器
    Beast::Server server(
        "0.0.0.0",
        DAS_HTTP_PORT,
        components.router,
        DAS::Http::g_server_condition.GetCondition());

    std::cout << "[DasHttp] Server running on port " << DAS_HTTP_PORT << std::endl;

    server.run();

    return DAS_S_OK;
}

int main(int argc, const char* argv[])
{
    std::cout << "[DasHttp] " << (argv[0] ? argv[0] : "") << " is start"
              << std::endl;

    DAS::Utils::SetCurrentThreadName(L"MAIN");

    const auto run_result = run();
    if (DAS::IsFailed(run_result))
    {
        return run_result;
    }

    return run_result;
}
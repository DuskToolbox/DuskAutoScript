#include <functional>
#include <iostream>
#include <ostream>

#include "das/DasApi.h"
#include "das/Utils/ThreadUtils.h"

#include "./AppComponent.hpp"
#include "./beast/Server.hpp"
#include "./controller/DasLogController.hpp"
#include "./controller/DasMiscController.hpp"
#include "./controller/DasPluginManagerController.hpp"
#include "./controller/DasProfileController.hpp"
#include "./controller/UISettingsController.hpp"
#include <boost/program_options.hpp>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <filesystem>

namespace Das::Http
{
    DAS_DEFINE_VARIABLE(g_server_condition){};

    void Das::Http::ServerCondition::RequestServerStop()
    {
        server_should_continue_ = false;
    }

    std::function<bool()> Das::Http::ServerCondition::GetCondition()
    {
        return [this]() -> bool { return server_should_continue_; };
    }

    DasResult run(const std::filesystem::path& plugin_dir)
    {
        Das::Http::AppComponent components(plugin_dir);

        // Cleanup plugins marked for deletion before starting HTTP server
        Das::Core::ForeignInterfaceHost::CleanupMarkedPlugins(plugin_dir);

        const auto init_result = InitializeDasCore();
        if (DAS::IsFailed(init_result))
        {
            std::cerr << "Init DAS Core failed. Error code = " << init_result
                      << std::endl;
            return init_result;
        }

        const auto port = DAS_HTTP_PORT;

        // 创建控制器实例
        auto misc_controller = std::make_shared<Das::Http::DasMiscController>();
        auto log_controller = std::make_shared<Das::Http::DasLogController>();
        auto profile_controller =
            std::make_shared<Das::Http::DasProfileController>(
                components.settings_manager);
        auto settings_controller =
            std::make_shared<Das::Http::DasUiSettingsController>(
                components.settings_manager);

        // 注册路由
        // Misc
        components.router->Post(
            DAS_HTTP_API_PREFIX "alive",
            [misc_controller](const Das::Http::Beast::HttpRequest& req)
            { return misc_controller->Alive(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "request_shutdown",
            [misc_controller](const Das::Http::Beast::HttpRequest& req)
            { return misc_controller->RequestShutdown(req); });

        // Log
        components.router->Post(
            DAS_HTTP_API_PREFIX "logs",
            [log_controller](const Das::Http::Beast::HttpRequest& req)
            { return log_controller->GetLogs(req); });

        // Profile
        components.router->Post(
            DAS_HTTP_API_PREFIX "profile/get",
            [profile_controller](const Das::Http::Beast::HttpRequest& req)
            { return profile_controller->GetProfileList(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "profile/create",
            [profile_controller](const Das::Http::Beast::HttpRequest& req)
            { return profile_controller->CreateProfile(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "profile/delete",
            [profile_controller](const Das::Http::Beast::HttpRequest& req)
            { return profile_controller->DeleteProfile(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "profile/{pid}/get",
            [profile_controller](const Das::Http::Beast::HttpRequest& req)
            { return profile_controller->GetProfile(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "profile/{pid}/update",
            [profile_controller](const Das::Http::Beast::HttpRequest& req)
            { return profile_controller->UpdateProfile(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "profile/{pid}/{guid}/get",
            [profile_controller](const Das::Http::Beast::HttpRequest& req)
            { return profile_controller->GetPluginSettings(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "profile/{pid}/{guid}/update",
            [profile_controller](const Das::Http::Beast::HttpRequest& req)
            { return profile_controller->UpdatePluginSettings(req); });

        // Plugin Manager
        auto plugin_controller =
            std::make_shared<Das::Http::DasPluginManagerController>(
                components.plugin_dir);
        components.router->Post(
            DAS_HTTP_API_PREFIX "plugin/list/get",
            [plugin_controller](const Das::Http::Beast::HttpRequest& req)
            { return plugin_controller->GetPluginList(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "plugin/update",
            [plugin_controller](const Das::Http::Beast::HttpRequest& req)
            { return plugin_controller->UpdatePlugin(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "plugin/{guid}/delete",
            [plugin_controller](const Das::Http::Beast::HttpRequest& req)
            { return plugin_controller->DeletePlugin(req); });

        // Settings
        components.router->Post(
            DAS_HTTP_API_PREFIX "settings/get",
            [settings_controller](const Das::Http::Beast::HttpRequest& req)
            { return settings_controller->V1SettingsGet(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "settings/update",
            [settings_controller](const Das::Http::Beast::HttpRequest& req)
            { return settings_controller->V1SettingsUpdate(req); });

        // 创建并启动服务器
        Das::Http::Beast::Server server(
            "0.0.0.0",
            port,
            components.router,
            g_server_condition.GetCondition());

        std::cout << "[DasHttp] Server running on port " << 8080 << std::endl;

        server.Run();

        return DAS_S_OK;
    }
}

int main(int argc, const char* argv[])
{
    std::cout << "[DasHttp] " << (argv[0] ? argv[0] : "") << " is start"
              << std::endl;

    DAS::Utils::SetCurrentThreadName(L"MAIN");

    boost::program_options::options_description desc("DasHttp Server");
    desc.add_options()("help,h", "Show help")(
        "plugin-dir",
        boost::program_options::value<std::string>()->default_value("plugins"),
        "Plugin directory path");

    boost::program_options::variables_map vm;
    boost::program_options::store(
        boost::program_options::parse_command_line(argc, argv, desc),
        vm);
    boost::program_options::notify(vm);

    std::filesystem::path plugin_dir = vm["plugin-dir"].as<std::string>();

    const auto run_result = Das::Http::run(plugin_dir);
    if (DAS::IsFailed(run_result))
    {
        return run_result;
    }

    return run_result;
}

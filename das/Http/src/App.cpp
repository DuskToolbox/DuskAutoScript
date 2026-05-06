#include <functional>
#include <iostream>
#include <string>

#include "das/DasApi.h"
#include "das/Utils/ThreadUtils.h"
#include "das/Utils/fmt.h"

#include "./AppComponent.hpp"
#include "./NotificationHub.hpp"
#include "./beast/Server.hpp"
#include "./controller/DasLogController.hpp"
#include "./controller/DasMiscController.hpp"
#include "./controller/DasPluginManagerController.hpp"
#include "./controller/DasProfileController.hpp"
#include "./controller/DasSchedulerController.hpp"
#include "./controller/UISettingsController.hpp"
#include <boost/program_options.hpp>
#include <filesystem>

#include "./service/DasPluginManagerServiceImpl.h"
#include "./service/DasProfileServiceImpl.h"
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/OcvWrapper/DasCVModuleEntry.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/abi/DasSettings.h>

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

        const auto port = DAS_HTTP_PORT;

        // Build settings_dir and plugin_dir as ABI-safe strings
        DasPtr<IDasReadOnlyString> p_settings_dir;
        auto settings_dir_str = std::filesystem::path("settings").u8string();
        auto sd_cr = CreateIDasReadOnlyStringFromUtf8(
            reinterpret_cast<const char*>(settings_dir_str.c_str()),
            p_settings_dir.Put());
        if (DAS::IsFailed(sd_cr))
        {
            DAS_LOG_ERROR("Failed to create settings_dir string");
            return sd_cr;
        }

        DasPtr<IDasReadOnlyString> p_plugin_dir;
        auto                       plugin_dir_str = plugin_dir.u8string();
        auto                       pd_cr = CreateIDasReadOnlyStringFromUtf8(
            reinterpret_cast<const char*>(plugin_dir_str.c_str()),
            p_plugin_dir.Put());
        if (DAS::IsFailed(pd_cr))
        {
            DAS_LOG_ERROR("Failed to create plugin_dir string");
            return pd_cr;
        }

        auto ipc_owner = DAS::Core::IPC::MainProcess::IpcContextPtr{
            DAS::Core::IPC::MainProcess::CreateIpcContext(false)};
        if (!ipc_owner)
        {
            DAS_LOG_ERROR("Failed to create IPC context");
            return DAS_E_FAIL;
        }
        auto* raw_ipc = ipc_owner.get();

        // Create CoreServices — IPC ownership transfers here
        auto cs_result = CreateIDasCoreServices(
            p_settings_dir.Get(),
            p_plugin_dir.Get(),
            raw_ipc,
            components.core_services.Put());
        static_cast<void>(ipc_owner.release());
        if (DAS::IsFailed(cs_result))
        {
            const auto message =
                std::string{"Failed to create CoreServices. Error code = "}
                + std::to_string(cs_result);
            DAS_LOG_ERROR(message.c_str());
            return cs_result;
        }

        // Obtain service interfaces through CoreServices
        auto ss_result = components.core_services->GetSettingsService(
            components.settings_service.Put());
        if (DAS::IsFailed(ss_result))
        {
            DAS_LOG_ERROR("Failed to get settings service");
            return ss_result;
        }

        auto pm_result = components.core_services->GetPluginManagerService(
            components.plugin_mgr_service.Put());
        if (DAS::IsFailed(pm_result))
        {
            DAS_LOG_ERROR("Failed to get plugin manager service");
            return pm_result;
        }

        auto sc_result = components.core_services->GetSchedulerService(
            components.scheduler_svc.Put());
        if (DAS::IsFailed(sc_result))
        {
            DAS_LOG_ERROR("Failed to get scheduler service");
            return sc_result;
        }

        // Set host exe path through service interface (not concrete class)
        const char* host_exe = std::getenv("DAS_HOST_EXE_PATH");
        if (host_exe && strlen(host_exe) > 0)
        {
            DasPtr<IDasReadOnlyString> p_host_path;
            auto                       hp_cr =
                CreateIDasReadOnlyStringFromUtf8(host_exe, p_host_path.Put());
            if (DAS::IsOk(hp_cr))
            {
                components.plugin_mgr_service->SetHostExePath(
                    p_host_path.Get());
            }
        }

        // IPC context for registration: get from CoreServices
        // CoreServices owns the IPC, we need a borrowed pointer for
        // ScopedCurrentIpcContext and thread launching.
        // We create a shared_ptr that does NOT own (null deleter) just for
        // the scope and thread — CoreServices is the real owner.
        // However, CreateIpcContext returned a raw pointer that was consumed
        // by CoreServices. We need to retrieve the IIpcContext pointer.
        // CoreServices wraps it with shared_ptr internally. We can get a raw
        // pointer from CoreServices for IPC registration and thread.
        //
        // Actually, we need the IpcContext for:
        // 1. ScopedCurrentIpcContext (needs IpcContext* for downcast)
        // 2. RegisterService calls
        // 3. ipc_context->Run() on a separate thread
        // 4. ipc_context->RequestStop() at shutdown
        //
        // The raw_ipc pointer is still valid since CoreServices holds it via
        // shared_ptr. We can use it directly for the scope/thread.
        auto ipc_context =
            std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext>(
                raw_ipc,
                [](DAS::Core::IPC::MainProcess::IIpcContext*)
                {
                    // No-op deleter: CoreServices owns the real lifetime
                });

        {
            DAS::Core::IPC::ScopedCurrentIpcContext scope(
                static_cast<DAS::Core::IPC::MainProcess::IpcContext*>(
                    ipc_context.get()));

            auto* profile_service = new DasProfileServiceImpl(
                *components.plugin_mgr_service,
                *components.settings_service);
            profile_service->AddRef();

            const auto reg_result = ipc_context->RegisterService(
                profile_service,
                DasIidOf<Das::ExportInterface::IDasProfileService>());

            if (DAS::IsFailed(reg_result))
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "Failed to register IDasProfileService. result = {}",
                        reg_result)
                        .c_str());
                profile_service->Release();
                return reg_result;
            }

            // Register IDasPluginManager service
            {
                auto* plugin_mgr_service_impl = new DasPluginManagerServiceImpl(
                    *components.plugin_mgr_service);
                plugin_mgr_service_impl->AddRef();

                const auto plugin_reg_result = ipc_context->RegisterService(
                    plugin_mgr_service_impl,
                    DasIidOf<Das::ExportInterface::IDasPluginManager>());

                if (DAS::IsFailed(plugin_reg_result))
                {
                    DAS_LOG_ERROR(
                        DAS_FMT_NS::format(
                            "Failed to register IDasPluginManager. result = {}",
                            plugin_reg_result)
                            .c_str());
                    plugin_mgr_service_impl->Release();
                    return plugin_reg_result;
                }
            }

            // Register computer vision services (cv.cpu / cv.cuda)
            {
                auto init_cv_result = InitializeDasCore(ipc_context.get());
                if (DAS::IsFailed(init_cv_result))
                {
                    DAS_LOG_ERROR(
                        DAS_FMT_NS::format(
                            "Failed to init CV services. result = {}",
                            init_cv_result)
                            .c_str());
                    return init_cv_result;
                }
            }
        }

        components.ipc_thread =
            std::thread([&ipc_context]() { ipc_context->Run(); });
        components.ipc_context = ipc_context;

        // Create controller instances
        auto misc_controller = std::make_shared<Das::Http::DasMiscController>();
        auto log_controller = std::make_shared<Das::Http::DasLogController>();
        auto profile_controller =
            std::make_shared<Das::Http::DasProfileController>(
                *components.settings_service);
        auto settings_controller =
            std::make_shared<Das::Http::DasUiSettingsController>(
                *components.settings_service);
        auto scheduler_controller =
            std::make_shared<Das::Http::DasSchedulerController>(
                *components.scheduler_svc,
                components.plugin_dir);

        // Register routes
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
                *components.plugin_mgr_service);
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

        // Scheduler
        components.router->Post(
            DAS_HTTP_API_PREFIX "scheduler/{profile}/initialize",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->Initialize(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "scheduler/{profile}/start",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->Start(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "scheduler/{profile}/stop",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->Stop(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "scheduler/{profile}/get",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->Get(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "scheduler/{profile}/{taskGuid}/put",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->AddTask(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "scheduler/{profile}/{taskId}/delete",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->DeleteTask(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX
            "scheduler/{profile}/{taskId}/properties/update",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->UpdateTaskProperties(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX
            "scheduler/{profile}/{taskId}/internal/properties/update",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            {
                return scheduler_controller->UpdateTaskInternalProperties(req);
            });

        // Create and start server
        Das::Http::Beast::Server server(
            "0.0.0.0",
            port,
            components.router,
            g_server_condition.GetCondition());

        // Create NotificationHub (needs io_context from Server)
        auto hub = std::make_shared<Das::Http::NotificationHub>(server.IoCtx());
        server.SetHub(hub);
        components.notification_hub = hub;

        // Wire notify callbacks through COM interfaces → WebSocket broadcast
        components.settings_service->SetSettingsNotifyCallback(
            [](const char* json_event, void* user_data)
            {
                auto* hub = static_cast<Das::Http::NotificationHub*>(user_data);
                if (hub && json_event)
                {
                    hub->Broadcast(std::string(json_event));
                }
            },
            hub.get());
        components.scheduler_svc->SetStateNotifyCallback(
            [](const char* json_state, void* user_data)
            {
                auto* hub = static_cast<Das::Http::NotificationHub*>(user_data);
                if (hub && json_state)
                {
                    hub->Broadcast(std::string(json_state));
                }
            },
            hub.get());

        std::cout << "[DasHttp] Server running on port " << port << std::endl;

        server.Run();

        // Shutdown IPC context
        if (components.ipc_context)
        {
            components.ipc_context->RequestStop();
        }
        if (components.ipc_thread.joinable())
        {
            components.ipc_thread.join();
        }

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

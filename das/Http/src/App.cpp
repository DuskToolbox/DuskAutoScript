#include <functional>
#include <iostream>
#include <optional>
#include <string>

#include "das/DasApi.h"
#include "das/Utils/PathText.h"
#include "das/Utils/ThreadUtils.h"
#include "das/Utils/fmt.h"
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasLocaleService.hpp>

#include <das/Debug/CrashDumper.h>

#include "./AppComponent.hpp"
#include "./NotificationHub.hpp"
#include "./SseHub.hpp"
#include "./beast/Server.hpp"
#include "./controller/DasImageController.hpp"
#include "./controller/DasLocaleController.hpp"
#include "./controller/DasLogController.hpp"
#include "./controller/DasMiscController.hpp"
#include "./controller/DasPluginManagerController.hpp"
#include "./controller/DasProfileController.hpp"
#include "./controller/DasSchedulerController.hpp"
#include "./controller/UISettingsController.hpp"
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>
#include <filesystem>
#include <memory>
#include <string_view>

#include "./service/DasPluginManagerServiceImpl.h"
#include "./service/DasProfileServiceImpl.h"
#include "./service/DasProgressServiceImpl.h"
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/HttpIpcServer.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/Logger/DasLogSessionImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/abi/DasSettings.h>

namespace Das::Http
{
    namespace
    {
        // 日志增量通知 tick 间隔（focus + debug
        // 共一套）：过短会放大"日志请求→写日志→ 触发
        // tick→新日志请求"的反馈循环密度；150ms 在刷新体感与负载间取平衡。
        constexpr std::chrono::milliseconds LOG_POLL_INTERVAL(150);
    }

    DAS_DEFINE_VARIABLE(g_server_condition){};

    void Das::Http::ServerCondition::RequestServerStop()
    {
        server_should_continue_ = false;
    }

    std::function<bool()> Das::Http::ServerCondition::GetCondition()
    {
        return [this]() -> bool { return server_should_continue_; };
    }

    DasResult run(
        const std::filesystem::path& plugin_dir,
        const std::filesystem::path& debug_dir,
        const std::optional<DAS::Core::IPC::MainProcess::WebSocketConfig>&
                           ws_config,
        const std::string& listen_address,
        int                listen_port)
    {
        Das::Http::AppComponent components(plugin_dir);

        // Build settings_dir and plugin_dir as ABI-safe strings
        DasPtr<IDasReadOnlyString> p_settings_dir;
        auto                       settings_dir_str = Das::Utils::PathToUtf8(
            std::filesystem::path{DAS_PATH_LITERAL("settings")});
        auto sd_cr = CreateIDasReadOnlyStringFromUtf8(
            settings_dir_str.c_str(),
            p_settings_dir.Put());
        if (DAS::IsFailed(sd_cr))
        {
            DAS_LOG_ERROR("Failed to create settings_dir string");
            return sd_cr;
        }

        DasPtr<IDasReadOnlyString> p_plugin_dir;
        auto plugin_dir_str = Das::Utils::PathToUtf8(plugin_dir);
        auto pd_cr = CreateIDasReadOnlyStringFromUtf8(
            plugin_dir_str.c_str(),
            p_plugin_dir.Put());
        if (DAS::IsFailed(pd_cr))
        {
            DAS_LOG_ERROR("Failed to create plugin_dir string");
            return pd_cr;
        }

        auto ipc_owner =
            [&]() -> Das::DasPtr<DAS::Core::IPC::MainProcess::IIpcContext>
        {
            if (ws_config)
            {
                std::cout
                    << "[DasHttp] --ipc-ws: creating WS-enabled IpcContext on "
                    << ws_config->listen_address << ":"
                    << ws_config->listen_port << std::endl;
                return DAS::Core::IPC::MainProcess::CreateIpcContext(
                    false,
                    *ws_config);
            }
            return DAS::Core::IPC::MainProcess::CreateIpcContext(false);
        }();

        if (!ipc_owner)
        {
            DAS_LOG_ERROR("Failed to create IPC context");
            return DAS_E_FAIL;
        }

        // 宿主对 IPC context 的用途（都是组合根职责）：
        // 1. 把 Http 层构造的服务发布进 IPC 全局服务表（依赖 SseHub /
        //    log session 等，只有宿主能构造），插件 Host 子进程经跨进程
        //    resolve 拿 proxy 调用；
        // 2. 起线程驱动 IPC 事件循环（下方 ipc_thread）；
        // 3. HTTP 业务派发进 BusinessThread（GIL / 插件生命周期线程亲和）；
        // 4. 关停时 RequestStop 停泵。
        // 与 CoreServices 各持一份引用（引用计数共享所有权），析构顺序
        // 无约束：最后一个 Release 触发销毁。
        auto cs_result = CreateIDasCoreServices(
            p_settings_dir.Get(),
            p_plugin_dir.Get(),
            ipc_owner.Get(),
            components.core_services.Put());
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

        // 业务路由经此调度器派发 BusinessThread 执行（覆盖 AppComponent
        // 里的占位路由——那个无调度器，全内联）
        components.router = std::make_shared<Das::Http::Beast::Router>(
            DAS::Core::IPC::schedule_on_business_thread(*ipc_owner));

        // 解析 IPC plugin host 子进程（DasHostX）路径：优先环境变量
        // DAS_HOST_EXE_PATH（ctest 测试注入），缺失则兜底为 cwd 下的
        // DasHostX。DasHttp 依赖 DasHostX 才能加载 loadMode=ipc 的插件，
        // 这是 DAS 自身的配置，路径解析由 DasHttp 兜底，不要求调用方
        // 注入环境变量。cwd 兜底与 DasHostX main.cpp 用 current_path()
        // 解析 plugin_dir 的约定一致；调用方启动 DasHttp 时 cwd 即其
        // 部署目录，二者同目录部署。
        // 环境变量值经原生宽度获取：Windows 窄 getenv 走 ACP，非 ASCII
        // 路径会损坏
        const auto env_host_exe =
            Das::Utils::PathFromEnvironment("DAS_HOST_EXE_PATH");
        // 两个来源（环境变量物化 / fallback）均以 NUL 结尾，data() 传
        // C API 安全
        std::string env_host_exe_text{
            env_host_exe ? Das::Utils::PathToUtf8(*env_host_exe) : ""};
        std::string_view host_exe_utf8{env_host_exe_text};
        std::string      host_exe_fallback;
        if (host_exe_utf8.empty())
        {
#if defined(_WIN32)
            constexpr std::u8string_view HOST_EXECUTABLE_NAME =
                u8"DasHostX.exe";
#else
            constexpr std::u8string_view HOST_EXECUTABLE_NAME = u8"DasHostX";
#endif
            host_exe_fallback = Das::Utils::PathToUtf8(
                std::filesystem::current_path()
                / std::filesystem::path{HOST_EXECUTABLE_NAME});
            std::cout << "[DasHttp] DAS_HOST_EXE_PATH not set; falling back to "
                      << host_exe_fallback << std::endl;
            host_exe_utf8 = host_exe_fallback;
        }

        if (!host_exe_utf8.empty())
        {
            DasPtr<IDasReadOnlyString> p_host_path;
            auto                       hp_cr = CreateIDasReadOnlyStringFromUtf8(
                host_exe_utf8.data(),
                p_host_path.Put());
            if (DAS::IsOk(hp_cr))
            {
                components.plugin_mgr_service->SetHostExePath(
                    p_host_path.Get());
            }
        }

        {
            const DAS::Core::IPC::ScopedCurrentIpcContext scope(
                static_cast<DAS::Core::IPC::MainProcess::IpcContext*>(
                    ipc_owner.Get()));

            auto* profile_service = new DasProfileServiceImpl(
                *components.plugin_mgr_service,
                *components.settings_service);
            profile_service->AddRef();

            const auto reg_result = ipc_owner->RegisterService(
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

                const auto plugin_reg_result = ipc_owner->RegisterService(
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

            // Register IDasSchedulerService as a named service (das.progress /
            // das.locale 范式)：IPC 模式插件组件（如 DasMaaPi 的仓库条目
            // 解析）经 ResolveMainProcessInterfaceByName("das.scheduler")
            // 取服务并编译仓库条目。注册表另持 COM 引用保活。
            {
                const auto scheduler_reg_result =
                    ipc_owner->RegisterServiceByName(
                        components.scheduler_svc.Get(),
                        DasIidOf<IDasSchedulerService>(),
                        "das.scheduler");
                if (DAS::IsFailed(scheduler_reg_result))
                {
                    DAS_LOG_ERROR(
                        DAS_FMT_NS::format(
                            "Failed to register IDasSchedulerService named "
                            "service. result = {}",
                            scheduler_reg_result)
                            .c_str());
                    return scheduler_reg_result;
                }
            }

            // Register computer vision services (cv.cpu / cv.cuda)
            {
                const auto debug_dir_str = Das::Utils::PathToUtf8(debug_dir);
                const DasCoreInitOptions init_options{debug_dir_str.c_str()};
                auto init_cv_result = InitializeDasCoreWithOptions(
                    ipc_owner.Get(),
                    &init_options);
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

        // IPC 事件循环（io_context / WS 监听）在独立线程驱动，关停在
        // run() 末尾 RequestStop + join。线程拷贝引用计数句柄（AddRef）。
        components.ipc_thread =
            std::thread([ipc_owner]() { ipc_owner->Run(); });

        // 焦点日志会话目录 + 统一通知器 + 主进程 recorder（注入式生命周期）：
        // session 是 RAII，run() 退出时 DasPtr<IDasLogSession> 析构递归删除
        // 会话目录（替代原 LogRecorderRuntime 静态全局 + Shutdown 调用）。
        // notifier 是 g_logger 的固定 sink（借用引用）。recorder 注入
        // notifier + session，写盘后 Signal(UserJsonl) 让 Http 16ms 定时器
        // 拉新广播。session 在 controllers 之前声明，保证 controllers 持
        // session 引用期间 session_dir 持续有效。
        DasPtr<Das::ExportInterface::IDasLogSession>         log_session;
        DasPtr<Das::ExportInterface::IDasLogNotifier>        log_notifier;
        DasPtr<Das::ExportInterface::IDasProcessLogRecorder> log_recorder_main;
        {
            const auto debug_dir_str = Das::Utils::PathToUtf8(debug_dir);
            DasPtr<IDasReadOnlyString> debug_dir_param;
            const auto dir_cr = CreateIDasReadOnlyStringFromUtf8(
                debug_dir_str.c_str(),
                debug_dir_param.Put());
            if (DAS::IsFailed(dir_cr))
            {
                DAS_LOG_ERROR(
                    "Failed to create debug_dir string for log session");
                return dir_cr;
            }
            const auto session_cr =
                CreateLogSession(debug_dir_param.Get(), log_session.Put());
            if (DAS::IsFailed(session_cr))
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "Failed to create log session. result = {}",
                        session_cr)
                        .c_str());
                return session_cr;
            }
            const auto notifier_cr = GetDasLogNotifier(log_notifier.Put());
            if (DAS::IsFailed(notifier_cr))
            {
                DAS_LOG_WARNING(
                    DAS_FMT_NS::format(
                        "DasLogNotifierSink not present ({}); log dirty flag "
                        "broadcasting disabled until logger initialized",
                        notifier_cr)
                        .c_str());
                // notifier 为空时 recorder 容忍（写盘不广播），不阻断启动。
            }
            const auto recorder_cr = CreateIDasProcessLogRecorder(
                log_notifier.Get(),
                log_session.Get(),
                log_recorder_main.Put());
            if (DAS::IsFailed(recorder_cr))
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "Failed to create main-process log recorder. result = "
                        "{}",
                        recorder_cr)
                        .c_str());
                return recorder_cr;
            }
        }

        // 把主进程 recorder 注册为具名服务（IDasProgressService 范式）：
        // IPC 模式插件（DasMaaPi）经 ResolveMainProcessInterfaceByName 拿
        // proxy， 经跨进程 marshal 调 Record → 主进程 BusinessThread 串行分发 →
        // recorder 写主进程 user.jsonl + Signal(UserJsonl) 置 dirty。
        if (log_recorder_main)
        {
            const DAS::Core::IPC::ScopedCurrentIpcContext scope(
                static_cast<DAS::Core::IPC::MainProcess::IpcContext*>(
                    ipc_owner.Get()));
            const auto recorder_reg = ipc_owner->RegisterServiceByName(
                log_recorder_main.Get(),
                DasIidOf<Das::ExportInterface::IDasProcessLogRecorder>(),
                "das.focus-log-recorder");
            if (DAS::IsFailed(recorder_reg))
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "Failed to register IDasProcessLogRecorder named "
                        "service. result = {}",
                        recorder_reg)
                        .c_str());
                return recorder_reg;
            }
        }

        // Create controller instances
        auto misc_controller = std::make_shared<Das::Http::DasMiscController>();
        auto log_controller =
            std::make_shared<Das::Http::DasLogController>(log_session.Get());
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

        // 基础设施端点留 io 内联（alive 探针独立性 / 停机时序）
        components.router->RegisterInline("POST", DAS_HTTP_API_PREFIX "alive");
        components.router->RegisterInline(
            "POST",
            DAS_HTTP_API_PREFIX "request_shutdown");

        // Log
        // 调试日志端点（3 段 logs/debug/{profile}）：返回 spdlog 文件的纯文本
        // logs[]。 注册在 2 段 logs/{profile} 之前——router
        // 按段数精确匹配不冲突（已读 router 源码确认：param_routes_
        // 要求 segments.size() 相同），但前置 注册让意图更显式。
        components.router->Post(
            DAS_HTTP_API_PREFIX "logs/debug/{profile}",
            [log_controller](const Das::Http::Beast::HttpRequest& req)
            { return log_controller->GetDebugLogs(req); });
        // 焦点日志端点（2 段 logs/{profile}）：返回 user.jsonl 的 LogEntry[]
        // JSONL。
        components.router->Post(
            DAS_HTTP_API_PREFIX "logs/{profile}",
            [log_controller](const Das::Http::Beast::HttpRequest& req)
            { return log_controller->GetLogs(req); });
        // 静态图片：按 imageRef（如 img/0001.jpg）从会话目录回 JPG。
        // DasImageController 改为实例方法以持 session（替代 LogRecorderRuntime
        // 静态调用），SanitizeImagePath 穿越防护不变。
        auto image_controller =
            std::make_shared<Das::Http::DasImageController>();
        image_controller->SetSession(log_session.Get());
        components.router->Get(
            DAS_HTTP_API_PREFIX "logs/{profile}/images/{file}",
            [image_controller](const Das::Http::Beast::HttpRequest& req)
            { return image_controller->GetImage(req); });

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
            DAS_HTTP_API_PREFIX "profile/{pid}/delete",
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
            DAS_HTTP_API_PREFIX "profile/{pid}/rename",
            [profile_controller](const Das::Http::Beast::HttpRequest& req)
            { return profile_controller->RenameProfile(req); });
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
            DAS_HTTP_API_PREFIX "scheduler/{profile}/repository/get",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->RepositoryGet(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "scheduler/{profile}/repository/entries",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->RepositoryCreate(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX
            "scheduler/{profile}/repository/entries/{entryId}/delete",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->RepositoryDelete(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX
            "scheduler/{profile}/repository/entries/{entryId}/rename",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->RepositoryRename(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX
            "scheduler/{profile}/repository/entries/{entryId}/authoring/get",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->RepositoryAuthoringGet(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX
            "scheduler/{profile}/repository/entries/{entryId}/authoring/apply",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->RepositoryAuthoringApply(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX
            "scheduler/{profile}/repository/entries/{entryId}/authoring/compile",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->RepositoryAuthoringCompile(req); });
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
        components.router->Post(
            DAS_HTTP_API_PREFIX "scheduler/{profile}/{taskId}/enable",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->EnableTask(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "scheduler/{profile}/{taskId}/disable",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->DisableTask(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX "scheduler/{profile}/tasks/{taskId}/runNow",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->RunTaskNow(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX
            "scheduler/{profile}/tasks/{taskId}/authoring/get",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->AuthoringGet(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX
            "scheduler/{profile}/tasks/{taskId}/authoring/apply",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->AuthoringApply(req); });
        components.router->Post(
            DAS_HTTP_API_PREFIX
            "scheduler/{profile}/tasks/{taskId}/authoring/compile",
            [scheduler_controller](const Das::Http::Beast::HttpRequest& req)
            { return scheduler_controller->AuthoringCompile(req); });

        // 把 server 的创建、回调装配与运行抽成局部 lambda：server 保持值
        // 对象、整个生命周期都在 lambda 体内。bind/listen 失败（端口被
        // Windows 动态排除端口范围占用、端口已被占用等）时 Listener 构造
        // 抛异常，由下方 try 统一捕获。这样错误处理与正常流程分离，try
        // 只包最小调用范围；IPC 清理在 try 外执行一次，正常退出与 bind
        // 失败共用，避免重复。
        auto run_server = [&]() -> DasResult
        {
            Das::Http::Beast::Server server(
                listen_address,
                listen_port,
                components.router,
                g_server_condition.GetCondition());

            // Create NotificationHub (needs io_context from Server)
            auto hub =
                std::make_shared<Das::Http::NotificationHub>(server.IoCtx());
            server.SetHub(hub);
            components.notification_hub = hub;

            // Create SSE hub (per-key SSE 广播，IDasProgressService 传输层)。
            auto sse_hub = std::make_shared<Das::Http::SseHub>(server.IoCtx());
            server.SetSseHub(sse_hub);

            // Register IDasProgressService 主进程服务（按名注册）。service
            // shared_ptr 以 Release 为 deleter 持有 COM 引用，注册表（DOM）
            // 另持有一份；run_server 结束后注册表持有保活，Shutdown 时释放。
            Das::Http::DasProgressServiceImpl* progress_raw =
                new Das::Http::DasProgressServiceImpl(sse_hub);
            progress_raw->AddRef();
            std::shared_ptr<Das::Http::DasProgressServiceImpl> progress_service(
                progress_raw,
                [](Das::Http::DasProgressServiceImpl* ptr) { ptr->Release(); });
            {
                const DAS::Core::IPC::ScopedCurrentIpcContext scope(
                    static_cast<DAS::Core::IPC::MainProcess::IpcContext*>(
                        ipc_owner.Get()));
                const auto progress_reg = ipc_owner->RegisterServiceByName(
                    progress_raw,
                    DasIidOf<Das::ExportInterface::IDasProgressService>(),
                    "das.progress");
                if (DAS::IsFailed(progress_reg))
                {
                    DAS_LOG_ERROR(
                        DAS_FMT_NS::format(
                            "Failed to register IDasProgressService. result = "
                            "{}",
                            progress_reg)
                            .c_str());
                    return progress_reg;
                }
            }
            // Register IDasLocaleService 主进程服务（按名注册 das.locale）。
            // 跨进程 locale 同步服务端：Host 子进程经 resolve das.locale 拿
            // proxy， AddCallback 注册反向通知、GetDefaultLocale 拉
            // locale、SetDefaultLocale 更新进程 GlobalLocale 并反向遍历
            // callbacks_。service shared_ptr 以 Release 为 deleter 持有 COM
            // 引用，DOM 另持一份保活（照 progress 范式）。
            Das::ExportInterface::IDasLocaleService* locale_raw = nullptr;
            if (DAS::IsFailed(CreateDasLocaleServiceImpl(&locale_raw))
                || locale_raw == nullptr)
            {
                DAS_LOG_ERROR("Failed to create IDasLocaleService");
                return DAS_E_FAIL;
            }
            std::shared_ptr<Das::ExportInterface::IDasLocaleService>
                locale_service(
                    locale_raw,
                    [](Das::ExportInterface::IDasLocaleService* ptr)
                    { ptr->Release(); });
            {
                const DAS::Core::IPC::ScopedCurrentIpcContext scope(
                    static_cast<DAS::Core::IPC::MainProcess::IpcContext*>(
                        ipc_owner.Get()));
                const auto locale_reg = ipc_owner->RegisterServiceByName(
                    locale_raw,
                    DasIidOf<Das::ExportInterface::IDasLocaleService>(),
                    "das.locale");
                if (DAS::IsFailed(locale_reg))
                {
                    DAS_LOG_ERROR(
                        DAS_FMT_NS::format(
                            "Failed to register IDasLocaleService. result = "
                            "{}",
                            locale_reg)
                            .c_str());
                    return locale_reg;
                }
            }

            // HTTP 入口触发 das.locale 服务（locale/put 设置、locale/get
            // 读取）。 controller 持 service 引用；路由 lambda 共享捕获
            // locale_service 保活。SetDefaultLocale 内部既更新主进程
            // GlobalLocale，又反向遍历 callbacks_ 下发子进程——同步链路由
            // service 既有逻辑保证，HTTP 层只触发。
            auto locale_controller =
                std::make_shared<Das::Http::DasLocaleController>(*locale_raw);
            components.router->Post(
                DAS_HTTP_API_PREFIX "locale/put",
                [locale_controller,
                 locale_service](const Das::Http::Beast::HttpRequest& req)
                { return locale_controller->PutLocale(req); });
            components.router->Post(
                DAS_HTTP_API_PREFIX "locale/get",
                [locale_controller,
                 locale_service](const Das::Http::Beast::HttpRequest& req)
                { return locale_controller->GetLocale(req); });
            // key 订阅归零 → service 反向通知（hub 业务中立，只带 key）。
            sse_hub->SetOnKeyEmpty(
                [weak_service =
                     std::weak_ptr<Das::Http::DasProgressServiceImpl>(
                         progress_service)](const std::string& key)
                {
                    if (auto service = weak_service.lock())
                    {
                        service->OnKeySubscribersEmpty(key);
                    }
                });
            // SSE 订阅 key 门：既无快照也无活跃上报者的 key 拒绝订阅——
            // 落普通路由（events 路径无注册路由 → 404），把「订了个死
            // key」从静默变成可诊断信号。service 已亡 = 主进程关闭中，
            // 放行无害（连接即将关闭）。
            server.SetSseKeyGate(
                [weak_service =
                     std::weak_ptr<Das::Http::DasProgressServiceImpl>(
                         progress_service)](const std::string& key)
                {
                    if (auto service = weak_service.lock())
                    {
                        return service->IsKeyKnown(key);
                    }
                    return true;
                });
            // 订阅建立处快照重放：会话注册完成后取该 key 的字段级快照
            // 逐条补发，再接直播帧（重放触发点由 SseSession 的
            // on_registered 保证在头写完 + hub Register 之后——注册前
            // 补发会与头写入并发交错损坏流；strand 串行使重放帧先于
            // 注册后直播帧）。查失败毫秒级跑赢订阅建立时 UI 不再卡在
            // 「正在检查更新」。session 经引用传入不持有；lambda 按值
            // 捕 weak_service 防悬垂。
            server.SetSseOnRegistered(
                [weak_service =
                     std::weak_ptr<Das::Http::DasProgressServiceImpl>(
                         progress_service)](
                    Das::Http::Beast::SseSessionBase& session)
                {
                    if (auto service = weak_service.lock())
                    {
                        for (const auto& payload :
                             service->TakeSnapshot(session.Key()))
                        {
                            session.WriteData(payload);
                        }
                    }
                });

            // Wire notify callbacks through COM interfaces → WebSocket
            // broadcast Core layer sends {"code","msg","data"}; we inject "api"
            // here.
            components.settings_service->SetSettingsNotifyCallback(
                [](const char* json_event, void* user_data)
                {
                    auto* hub =
                        static_cast<Das::Http::NotificationHub*>(user_data);
                    if (!hub || !json_event)
                    {
                        return;
                    }
                    auto parsed = Das::Utils::ParseYyjsonFromString(json_event);
                    if (!parsed)
                    {
                        return;
                    }
                    auto obj_opt = parsed->as_object();
                    if (obj_opt)
                    {
                        obj_opt.value()[std::string_view("api")] =
                            std::string_view("api/v1/settings/get");
                    }
                    auto serialized =
                        Das::Utils::SerializeYyjsonValue(*parsed, false);
                    if (serialized)
                    {
                        hub->Broadcast(std::move(*serialized));
                    }
                },
                hub.get());
            components.scheduler_svc->SetStateNotifyCallback(
                [](const char* json_state, void* user_data)
                {
                    auto* hub =
                        static_cast<Das::Http::NotificationHub*>(user_data);
                    if (!hub || !json_state)
                    {
                        return;
                    }
                    auto parsed = Das::Utils::ParseYyjsonFromString(json_state);
                    if (!parsed)
                    {
                        return;
                    }
                    auto obj_opt = parsed->as_object();
                    if (obj_opt)
                    {
                        obj_opt.value()[std::string_view("api")] =
                            std::string_view("api/v1/scheduler/get");
                    }
                    auto serialized =
                        Das::Utils::SerializeYyjsonValue(*parsed, false);
                    if (serialized)
                    {
                        hub->Broadcast(std::move(*serialized));
                    }
                },
                hub.get());

            // 日志增量统一通知（focus + debug 共一套机制）：Http 150ms 定时器挂
            // 在 server 的 io_context（非 IPC BusinessThread——Consume 不得占
            // RPC 分发线程）。每 tick 轮询两个 channel 的 dirty flag：
            //   - Spdlog dirty：sink::sink_it_ 在每条 DAS_LOG 触发时置；
            //   - UserJsonl dirty：主进程 recorder Record 写盘后
            //   Signal(UserJsonl)。
            // 任一 dirty 时 hub->Broadcast 一个无 payload 的对应 api tick
            // （api/v1/logs/debug/get 或 api/v1/logs/get），前端按 api 路由
            // Refresh→ReadAfter 主动拉。Consume 是纯 atomic exchange，可安全在
            // io_context 线程调。timer 在 server.Run() 退出后 cancel。
            auto log_poll_timer = std::make_shared<boost::asio::steady_timer>(
                server.IoCtx(),
                LOG_POLL_INTERVAL);
            auto recursive_tick = std::make_shared<
                std::function<void(const boost::system::error_code&)>>();
            *recursive_tick = [hub, log_poll_timer, recursive_tick](
                                  const boost::system::error_code& ec)
            {
                if (ec)
                {
                    return; // timer canceled（server shutdown）
                }
                Das::DasPtr<Das::ExportInterface::IDasLogNotifier> notifier;
                if (GetDasLogNotifier(notifier.Put()) == DAS_S_OK && notifier)
                {
                    bool was_dirty = false;
                    if (notifier->Consume(
                            Das::ExportInterface::DAS_LOG_CHANNEL_SPDLOG,
                            &was_dirty)
                        == DAS_S_OK)
                    {
                        if (was_dirty && hub)
                        {
                            // tick payload 不带 entries：前端按 api 路由
                            // Refresh→ReadAfter。
                            hub->Broadcast(
                                std::string{"{\"api\":\"api/v1/logs/debug/"
                                            "get\",\"code\":0,\"data\":{}}"});
                        }
                    }
                    was_dirty = false;
                    if (notifier->Consume(
                            Das::ExportInterface::DAS_LOG_CHANNEL_USER_JSONL,
                            &was_dirty)
                        == DAS_S_OK)
                    {
                        if (was_dirty && hub)
                        {
                            hub->Broadcast(
                                std::string{"{\"api\":\"api/v1/logs/"
                                            "get\",\"code\":0,\"data\":{}}"});
                        }
                    }
                }
                log_poll_timer->expires_after(LOG_POLL_INTERVAL);
                log_poll_timer->async_wait(*recursive_tick);
            };
            log_poll_timer->async_wait(*recursive_tick);

            std::cout << "[DasHttp] Server listening on " << listen_address
                      << ":" << listen_port << std::endl;

            server.Run();

            // server.Run() 返回（RequestServerStop 触发）：cancel pending
            // async_wait， 让 recursive_tick 收 operation_aborted
            // 后停止重排，不再访问 io_context。
            log_poll_timer->cancel();

            // 停 HTTP 后立即无效化 hub（切断 IPC→HTTP 的 SSE 写路径）：之后
            // 析构 server（io_context）前，IPC 线程晚到的 Report 经
            // hub->Broadcast 变 no-op，不再 dispatch 到已析构 io_context。
            // 过渡期（RequestServerStop → server.Run 返回）IPC 写 dispatch 到
            // 还在 run 的 ioc，无害。
            sse_hub->Invalidate();

            // ioc 随 Server 栈帧析构，完成回调的 post 须先排空
            components.router->WaitBusinessDrained();

            // 会话目录由外层 DasPtr<IDasLogSession> log_session 析构 RAII
            // 删除， 无需 Shutdown 调用（替代原
            // LogRecorderRuntime::Shutdown）。
            return DAS_S_OK;
        };

        DasResult run_result = DAS_E_FAIL;
        try
        {
            run_result = run_server();
        }
        catch (const std::exception& ex)
        {
            // bind/listen 失败（典型：端口落在 Windows 动态排除端口范围，
            // bind 返回 WSAEACCES）。打印明确错误，避免之前 bind 失败却照常
            // cout "listening"、io_context 无挂起 work 秒退、外部只见神秘
            // 「连接失败」的假监听现象。
            const auto msg =
                std::string{"[DasHttp] 监听端口失败，服务器未启动: "}
                + ex.what();
            std::cerr << msg << std::endl;
            DAS_LOG_ERROR(msg.c_str());
        }

        // bind 失败路径：log_session 的 DasPtr 析构 RAII 删除会话目录
        // （幂等，run_server 未执行时 session 也已构造）。

        // 停 IPC（排空在途 Report）：service 仍由注册表（DOM）保活，join 安全。
        ipc_owner->RequestStop();
        if (components.ipc_thread.joinable())
        {
            components.ipc_thread.join();
        }

        // IPC 停止后才释放 service 注册表（core Shutdown），确保无晚到 Report
        // 打已析构 service。bind 失败路径（server 未启动）也统一在此清理。
        // PluginManager 卸载（ShutdownCoreRuntime 内）会派发到 BusinessThread：
        // BT 由 ~IpcContext 停止（CoreServicesImpl
        // 成员析构阶段，晚于其函数体）， 此处与 components 析构两条路径 BT
        // 均存活，派发必达。
        const auto shutdown_result = components.core_services->Shutdown();
        if (DAS::IsFailed(shutdown_result))
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "Failed to shutdown CoreServices. result = {}",
                    shutdown_result)
                    .c_str());
        }

        return run_result;
    }
}

namespace
{
    struct ListenEndpoint
    {
        std::string address;
        int         port;
    };

    std::optional<ListenEndpoint> ParseListenEndpoint(const std::string& value)
    {
        const auto colon_pos = value.rfind(':');
        if (colon_pos == std::string::npos || colon_pos == 0
            || colon_pos == value.size() - 1)
        {
            return std::nullopt;
        }
        try
        {
            auto addr_str = value.substr(0, colon_pos);
            if (addr_str.size() >= 2 && addr_str.front() == '['
                && addr_str.back() == ']')
            {
                addr_str = addr_str.substr(1, addr_str.size() - 2);
            }
            const auto addr = boost::asio::ip::make_address(addr_str);
            const int  port = std::stoi(value.substr(colon_pos + 1));
            if (port <= 0 || port > 65535)
            {
                return std::nullopt;
            }
            return ListenEndpoint{addr.to_string(), port};
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }
}

int main(int argc, const char* argv[])
{
    Das::Debug::InstallCrashDumper();

    std::cout << "[DasHttp] " << (argv[0] ? argv[0] : "") << " is start"
              << std::endl;

    DAS::Utils::SetCurrentThreadName(L"MAIN");

    boost::program_options::options_description desc("DasHttp Server");
    desc.add_options()("help,h", "Show help")(
        "plugin-dir",
        boost::program_options::value<std::string>()->default_value("plugins"),
        "Plugin directory path")(
        "debug-dir",
        boost::program_options::value<std::string>()->default_value(
            "logs/debug"),
        "Debug artifact directory path")(
        "ipc-ws",
        boost::program_options::value<std::string>(),
        "Enable WebSocket IPC server (format: ip:port, e.g. 0.0.0.0:9527)")(
        "listen",
        boost::program_options::value<std::string>()->default_value(
            "127.0.0.1:" + std::to_string(DAS_HTTP_PORT)),
        "HTTP listen address (format: ip:port, e.g. 127.0.0.1:" DAS_STR(
            DAS_HTTP_PORT) ")");

    boost::program_options::variables_map vm;
    boost::program_options::store(
        boost::program_options::parse_command_line(argc, argv, desc),
        vm);
    boost::program_options::notify(vm);

    const std::filesystem::path plugin_dir =
        Das::Utils::PathFromUtf8(vm["plugin-dir"].as<std::string>());
    const std::filesystem::path debug_dir =
        Das::Utils::PathFromUtf8(vm["debug-dir"].as<std::string>());

    std::optional<DAS::Core::IPC::MainProcess::WebSocketConfig> ws_config;
    if (vm.count("ipc-ws"))
    {
        const auto ws_value = vm["ipc-ws"].as<std::string>();
        auto       endpoint = ParseListenEndpoint(ws_value);
        if (!endpoint)
        {
            std::cerr << "[DasHttp] Invalid --ipc-ws value: " << ws_value
                      << std::endl;
            return 1;
        }
        ws_config.emplace();
        ws_config->listen_address = endpoint->address;
        ws_config->listen_port = static_cast<uint16_t>(endpoint->port);
    }

    std::string listen_address = "127.0.0.1";
    int         listen_port = DAS_HTTP_PORT;
    if (vm.count("listen"))
    {
        const auto listen_value = vm["listen"].as<std::string>();
        auto       endpoint = ParseListenEndpoint(listen_value);
        if (!endpoint)
        {
            std::cerr << "[DasHttp] Invalid --listen value: " << listen_value
                      << std::endl;
            return 1;
        }
        listen_address = endpoint->address;
        listen_port = endpoint->port;
    }

    const auto run_result = Das::Http::run(
        plugin_dir,
        debug_dir,
        ws_config,
        listen_address,
        listen_port);
    if (DAS::IsFailed(run_result))
    {
        return run_result;
    }

    return run_result;
}

// DAS Host Process - IPC Host Entry Point
// B8 Host 进程模型实现

#include <algorithm> // std::transform, std::tolower
#include <atomic>
#include <boost/program_options.hpp>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/IPC/HandshakeSerialization.h>
#include <das/Core/IPC/Host/HostCommandHandlers.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/IDistributedObjectManager.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// Global IPC context pointer for signal handler
static DAS::Core::IPC::Host::IIpcContext* g_ipc_context = nullptr;

// 信号处理函数
static std::atomic<bool> g_shutdown_requested{false};

void SignalHandler(int signal)
{
    (void)signal;
    g_shutdown_requested.store(true);
    if (g_ipc_context)
    {
        g_ipc_context->RequestStop();
    }
}

namespace
{
    // 序列化辅助函数来自 HandshakeSerialization.h

    static DAS::DasPtr<DAS::Core::ForeignInterfaceHost::IForeignLanguageRuntime>
        g_runtime;

    auto LoadPluginWithNativeRuntime(const std::filesystem::path& manifest_path)
        -> DAS::Utils::Expected<DAS::DasPtr<IDasBase>>
    {
        std::ifstream json_file(manifest_path);
        if (!json_file.is_open())
        {
            std::string msg = DAS_FMT_NS::format(
                "无法打开 manifest 文件: {}",
                manifest_path.string());
            DAS_LOG_ERROR(msg.c_str());
            return tl::make_unexpected(DAS_E_IPC_PLUGIN_LOAD_FAILED);
        }

        std::string manifest_content(
            (std::istreambuf_iterator<char>(json_file)),
            std::istreambuf_iterator<char>());
        auto manifest_json_opt =
            Das::Utils::ParseYyjsonFromString(manifest_content);
        if (!manifest_json_opt)
        {
            std::string msg = DAS_FMT_NS::format(
                "解析 manifest JSON 失败: {}",
                manifest_path.string());
            DAS_LOG_ERROR(msg.c_str());
            return tl::make_unexpected(DAS_E_IPC_PLUGIN_LOAD_FAILED);
        }
        yyjson::value manifest_json = std::move(*manifest_json_opt);

        std::string plugin_language;
        {
            auto manifest_obj = manifest_json.as_object();
            if (!manifest_obj)
            {
                std::string msg =
                    "提取插件信息失败: manifest root is not an object";
                DAS_LOG_ERROR(msg.c_str());
                return tl::make_unexpected(DAS_E_IPC_PLUGIN_LOAD_FAILED);
            }
            auto language_val = (*manifest_obj)[std::string_view("language")];
            auto language_str = language_val.as_string();
            if (!language_str)
            {
                std::string msg =
                    "提取插件信息失败: manifest missing 'language' field";
                DAS_LOG_ERROR(msg.c_str());
                return tl::make_unexpected(DAS_E_IPC_PLUGIN_LOAD_FAILED);
            }
            plugin_language = std::string(*language_str);
        }

        std::string lang_lower;
        std::transform(
            plugin_language.begin(),
            plugin_language.end(),
            std::back_inserter(lang_lower),
            [](unsigned char c) { return std::tolower(c); });

        if (!g_runtime)
        {
            DAS::Core::ForeignInterfaceHost::ForeignLanguageRuntimeFactoryDesc
                desc;

            if (lang_lower == "python")
            {
                desc.language = DAS::Core::ForeignInterfaceHost::
                    ForeignInterfaceLanguage::Python;
            }
            else if (lang_lower == "java")
            {
                desc.language = DAS::Core::ForeignInterfaceHost::
                    ForeignInterfaceLanguage::Java;
            }
            else if (lang_lower == "lua")
            {
                desc.language = DAS::Core::ForeignInterfaceHost::
                    ForeignInterfaceLanguage::Lua;
            }
            else if (lang_lower == "csharp")
            {
                desc.language = DAS::Core::ForeignInterfaceHost::
                    ForeignInterfaceLanguage::CSharp;
            }
            else
            {
                desc.language = DAS::Core::ForeignInterfaceHost::
                    ForeignInterfaceLanguage::Cpp;
            }

            auto result =
                DAS::Core::ForeignInterfaceHost::CreateForeignLanguageRuntime(
                    desc);
            if (result.has_value())
            {
                g_runtime = std::move(result.value());
                std::string msg = DAS_FMT_NS::format(
                    "Runtime initialized: {}",
                    plugin_language);
                DAS_LOG_INFO(msg.c_str());
            }
            else
            {
                std::string msg = DAS_FMT_NS::format(
                    "Failed to create runtime: {}",
                    plugin_language);
                DAS_LOG_ERROR(msg.c_str());
                return tl::make_unexpected(DAS_E_IPC_PLUGIN_LOAD_FAILED);
            }
        }

        auto result = g_runtime->LoadPlugin(manifest_path);
        if (!result.has_value())
        {
            std::string err_msg = DAS_FMT_NS::format(
                "Plugin load failed: {}",
                manifest_path.string());
            DAS_LOG_ERROR(err_msg.c_str());
            return tl::make_unexpected(DAS_E_IPC_PLUGIN_LOAD_FAILED);
        }

        return result.value();
    }
} // namespace

int main(int argc, char* argv[])
{
    try
    {
        boost::program_options::options_description desc(
            "DAS Host Process - IPC Resource Owner");
        desc.add_options()("verbose,v", "Enable verbose logging")(
            "help,h",
            "Show this help message")(
            "log-level",
            boost::program_options::value<std::string>()->default_value(""),
            "Set log level: trace, debug, info, warn, error, critical, off")(
            "main-pid",
            boost::program_options::value<uint32_t>(),
            "Main process PID (enables Named Pipe connect mode)")(
            "connect-url",
            boost::program_options::value<std::string>(),
            "Main process WebSocket URL (enables HTTP transport mode). "
            "Example: ws://localhost:9527")(
            "plugin-dir",
            boost::program_options::value<std::string>(),
            "Plugin directory path for remote Host mode (LIST_FILE/READ_FILE scoping)");

        boost::program_options::variables_map vm;
        boost::program_options::store(
            boost::program_options::parse_command_line(argc, argv, desc),
            vm);
        boost::program_options::notify(vm);

        if (vm.count("help"))
        {
            std::ostringstream oss;
            oss << desc;
            std::string help_msg = oss.str();
            DAS_LOG_INFO(help_msg.c_str());
            return EXIT_SUCCESS;
        }

        bool verbose = vm.count("verbose") > 0;

        // Parse --log-level argument
        if (vm.count("log-level"))
        {
            std::string log_level_str = vm["log-level"].as<std::string>();
            int         level = -1;
            if (log_level_str == "trace")
            {
                level = DAS_LOG_LEVEL_TRACE;
            }
            else if (log_level_str == "debug")
            {
                level = DAS_LOG_LEVEL_DEBUG;
            }
            else if (log_level_str == "info")
            {
                level = DAS_LOG_LEVEL_INFO;
            }
            else if (log_level_str == "warn")
            {
                level = DAS_LOG_LEVEL_WARN;
            }
            else if (log_level_str == "error")
            {
                level = DAS_LOG_LEVEL_ERROR;
            }
            else if (log_level_str == "critical")
            {
                level = DAS_LOG_LEVEL_CRITICAL;
            }
            else if (log_level_str == "off")
            {
                level = DAS_LOG_LEVEL_OFF;
            }

            if (level >= 0)
            {
                DasSetLogLevel(level);
            }
        }

        // Handle --verbose as alias for --log-level=debug
        if (verbose)
        {
            DasSetLogLevel(DAS_LOG_LEVEL_DEBUG);
        }

        // Fallback: read DAS_LOG_LEVEL environment variable
        // (CLI --log-level takes precedence over env var)
        const char* env_log_level = std::getenv("DAS_LOG_LEVEL");
        if (env_log_level != nullptr
            && vm["log-level"].as<std::string>().empty() && !verbose)
        {
            int         level = -1;
            std::string env_str(env_log_level);
            if (env_str == "trace")
            {
                level = DAS_LOG_LEVEL_TRACE;
            }
            else if (env_str == "debug")
            {
                level = DAS_LOG_LEVEL_DEBUG;
            }
            else if (env_str == "info")
            {
                level = DAS_LOG_LEVEL_INFO;
            }
            else if (env_str == "warn")
            {
                level = DAS_LOG_LEVEL_WARN;
            }
            else if (env_str == "error")
            {
                level = DAS_LOG_LEVEL_ERROR;
            }
            else if (env_str == "critical")
            {
                level = DAS_LOG_LEVEL_CRITICAL;
            }
            else if (env_str == "off")
            {
                level = DAS_LOG_LEVEL_OFF;
            }

            if (level >= 0)
            {
                DasSetLogLevel(level);
            }
        }

        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);
#ifdef SIGBREAK
        std::signal(SIGBREAK, SignalHandler);
#endif

        DAS_LOG_INFO("DAS Host Process starting...");

        // 创建 IPC Context
        DAS::Core::IPC::Host::IpcContextConfig config{};
        std::string                            connect_url_storage;

        if (vm.count("connect-url"))
        {
            connect_url_storage = vm["connect-url"].as<std::string>();
            config.connect_url = connect_url_storage.c_str();
            DAS_LOG_INFO(
                std::format(
                    "Host process running in HTTP mode, connect URL: {}",
                    connect_url_storage)
                    .c_str());
        }
        else if (vm.count("main-pid"))
        {
            config.main_pid = vm["main-pid"].as<uint32_t>();
            DAS_LOG_INFO(
                std::format(
                    "Host process running in PIPE mode, main PID: {}",
                    config.main_pid)
                    .c_str());
        }
        else
        {
            DAS_LOG_ERROR(
                "Either --main-pid or --connect-url must be specified");
            return EXIT_FAILURE;
        }

        DAS::Core::IPC::Host::IpcContextPtr ctx{
            DAS::Core::IPC::Host::CreateIpcContext(config)};
        if (!ctx)
        {
            DAS_LOG_ERROR("Failed to create IPC context");
            return EXIT_FAILURE;
        }

        g_ipc_context = ctx.get();

        DAS::Core::IPC::Host::HostCommandHandlerOptions handler_options{};
        handler_options.load_plugin = LoadPluginWithNativeRuntime;

        if (vm.count("plugin-dir"))
        {
            handler_options.plugin_dir =
                std::filesystem::path(vm["plugin-dir"].as<std::string>());
        }
        else
        {
            handler_options.plugin_dir = std::filesystem::current_path();
            DAS_LOG_WARNING(
                std::format(
                    "--plugin-dir not specified, falling back to working "
                    "directory: {}",
                    handler_options.plugin_dir.string())
                    .c_str());
        }

        const DasResult handler_result =
            DAS::Core::IPC::Host::RegisterHostCommandHandlers(
                ctx.get(),
                std::move(handler_options));
        if (DAS::IsFailed(handler_result))
        {
            std::string err_msg = DAS_FMT_NS::format(
                "Failed to register host command handlers: {}",
                handler_result);
            DAS_LOG_ERROR(err_msg.c_str());
            return EXIT_FAILURE;
        }

        // 运行 IPC 事件循环
        DasResult result = ctx->Run();

        g_ipc_context = nullptr;

        if (DAS::IsFailed(result))
        {
            std::string err_msg =
                DAS_FMT_NS::format("IPC context run failed: {}", result);
            DAS_LOG_ERROR(err_msg.c_str());
            return EXIT_FAILURE;
        }
        DAS_LOG_INFO("DAS Host Process shutdown complete");
        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        std::string err_msg = DAS_FMT_NS::format("Error: {}", e.what());
        DAS_LOG_ERROR(err_msg.c_str());
        return EXIT_FAILURE;
    }
}

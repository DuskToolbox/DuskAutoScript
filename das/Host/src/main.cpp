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
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/IDistributedObjectManager.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>
#include <das/Utils/fmt.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
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
}

// 注册 LOAD_PLUGIN 处理器
void RegisterLoadPluginHandler(DAS::Core::IPC::Host::IIpcContext* ctx)
{
    ctx->RegisterCommandHandler(
        static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::LOAD_PLUGIN),
        [ctx](
            const DAS::Core::IPC::ValidatedIPCMessageHeader& header,
            std::span<const uint8_t>                       payload,
            DAS::Core::IPC::IpcCommandResponse&          response) -> DasResult
        {
            (void)header;

            std::string manifest_path;
            size_t      offset = 0;
            if (!DAS::Core::IPC::DeserializeString(
                    payload,
                    offset,
                    manifest_path))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            std::ifstream json_file(manifest_path);
            if (!json_file.is_open())
            {
                std::string msg = DAS_FMT_NS::format(
                    "无法打开 manifest 文件: {}",
                    manifest_path);
                DAS_LOG_ERROR(msg.c_str());
                response.error_code = DAS_E_IPC_PLUGIN_LOAD_FAILED;
                response.response_data.clear();
                return DAS_E_IPC_PLUGIN_LOAD_FAILED;
            }

            nlohmann::json manifest_json;
            try
            {
                json_file >> manifest_json;
            }
            catch (const nlohmann::json::exception& e)
            {
                std::string msg = DAS_FMT_NS::format(
                    "解析 manifest JSON 失败: {} - {}",
                    manifest_path,
                    e.what());
                DAS_LOG_ERROR(msg.c_str());
                response.error_code = DAS_E_IPC_PLUGIN_LOAD_FAILED;
                response.response_data.clear();
                return DAS_E_IPC_PLUGIN_LOAD_FAILED;
            }

            std::string plugin_name;
            std::string plugin_extension;
            std::string plugin_language;
            try
            {
                manifest_json["name"].get_to(plugin_name);
                manifest_json["pluginFilenameExtension"].get_to(
                    plugin_extension);
                manifest_json["language"].get_to(plugin_language);
            }
            catch (const nlohmann::json::exception& e)
            {
                std::string msg =
                    DAS_FMT_NS::format("提取插件信息失败: {}", e.what());
                DAS_LOG_ERROR(msg.c_str());
                response.error_code = DAS_E_IPC_PLUGIN_LOAD_FAILED;
                response.response_data.clear();
                return DAS_E_IPC_PLUGIN_LOAD_FAILED;
            }

            std::filesystem::path manifest_dir =
                std::filesystem::path(manifest_path).parent_path();

            std::string lang_lower;
            std::transform(
                plugin_language.begin(),
                plugin_language.end(),
                std::back_inserter(lang_lower),
                [](unsigned char c) { return std::tolower(c); });

            if (!g_runtime)
            {
                std::filesystem::path runtime_path =
                    manifest_dir / (plugin_name + "." + plugin_extension);

                DAS::Core::ForeignInterfaceHost::
                    ForeignLanguageRuntimeFactoryDesc desc;

                DAS::Core::ForeignInterfaceHost::JavaRuntimeDescPtr java_desc;

                if (lang_lower == "java")
                {
                    desc.language = DAS::Core::ForeignInterfaceHost::
                        ForeignInterfaceLanguage::Java;
                    java_desc.reset(
                        DAS::Core::ForeignInterfaceHost::
                            CreateJavaRuntimeDesc());
                    java_desc->SetClassPath({runtime_path});
                    desc.p_user_data = java_desc.get();
                }
                else
                {
                    desc.language = DAS::Core::ForeignInterfaceHost::
                        ForeignInterfaceLanguage::Cpp;
                    desc.p_user_data = nullptr;
                }

                auto result = DAS::Core::ForeignInterfaceHost::
                    CreateForeignLanguageRuntime(desc);
                if (result.has_value())
                {
                    g_runtime = std::move(result.value());
                    std::string msg = DAS_FMT_NS::format(
                        "运行时初始化完成: {}",
                        plugin_language);
                    DAS_LOG_INFO(msg.c_str());
                }
                else
                {
                    std::string msg = DAS_FMT_NS::format(
                        "创建运行时失败: {}",
                        plugin_language);
                    DAS_LOG_ERROR(msg.c_str());
                    response.error_code = DAS_E_IPC_PLUGIN_LOAD_FAILED;
                    response.response_data.clear();
                    return DAS_E_IPC_PLUGIN_LOAD_FAILED;
                }
            }

            std::filesystem::path plugin_path;
            if (lang_lower == "java")
            {
                plugin_path = manifest_path;
            }
            else
            {
                plugin_path =
                    manifest_dir / (plugin_name + "." + plugin_extension);
            }

            std::string msg =
                DAS_FMT_NS::format("加载插件: {}", plugin_path.string());
            DAS_LOG_INFO(msg.c_str());

            auto result = g_runtime->LoadPlugin(plugin_path.string());
            if (!result.has_value())
            {
                msg = DAS_FMT_NS::format(
                    "插件加载失败: {}",
                    plugin_path.string());
                DAS_LOG_ERROR(msg.c_str());
                response.error_code = DAS_E_IPC_PLUGIN_LOAD_FAILED;
                response.response_data.clear();
                return DAS_E_IPC_PLUGIN_LOAD_FAILED;
            }

            auto                     plugin_ptr = result.value();
            DAS::Core::IPC::ObjectId object_id;
            DasResult                reg_result =
                ctx->RegisterLocalObject(plugin_ptr.Get(), object_id);

            if (DAS::IsFailed(reg_result))
            {
                std::string msg = DAS_FMT_NS::format(
                    "[LOAD_PLUGIN] 对象注册失败: {:#x}",
                    static_cast<uint32_t>(reg_result));
                DAS_LOG_ERROR(msg.c_str());
                response.error_code = reg_result;
                response.response_data.clear();
                return reg_result;
            }

            response.error_code = DAS_S_OK;

            response.response_data.push_back(object_id.session_id & 0xFF);
            response.response_data.push_back(
                (object_id.session_id >> 8) & 0xFF);
            response.response_data.push_back(object_id.generation & 0xFF);
            response.response_data.push_back(
                (object_id.generation >> 8) & 0xFF);
            response.response_data.push_back(object_id.local_id & 0xFF);
            response.response_data.push_back((object_id.local_id >> 8) & 0xFF);
            response.response_data.push_back((object_id.local_id >> 16) & 0xFF);
            response.response_data.push_back((object_id.local_id >> 24) & 0xFF);

            const auto& iid = DAS_IID_BASE;
            response.response_data.insert(
                response.response_data.end(),
                reinterpret_cast<const uint8_t*>(&iid),
                reinterpret_cast<const uint8_t*>(&iid) + sizeof(DasGuid));

            response.response_data.push_back(object_id.session_id & 0xFF);
            response.response_data.push_back(
                (object_id.session_id >> 8) & 0xFF);

            uint16_t version = 1;
            response.response_data.push_back(version & 0xFF);
            response.response_data.push_back((version >> 8) & 0xFF);

            std::string log_msg = DAS_FMT_NS::format(
                "[LOAD_PLUGIN] 插件已加载, object_id={{session:{}, gen:{}, local:{}}}",
                object_id.session_id,
                object_id.generation,
                object_id.local_id);
            DAS_LOG_INFO(log_msg.c_str());
            return DAS_S_OK;
        });
}

int main(int argc, char* argv[])
{
    try
    {
        boost::program_options::options_description desc(
            "DAS Host Process - IPC Resource Owner");
        desc.add_options()("verbose,v", "Enable verbose logging")(
            "help,h",
            "Show this help message")(
            "main-pid",
            boost::program_options::value<uint32_t>(),
            "Main process PID (enables connect mode)");

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
        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);
#ifdef SIGBREAK
        std::signal(SIGBREAK, SignalHandler);
#endif

        DAS_LOG_INFO("DAS Host Process starting...");

        // 创建 IPC Context
        DAS::Core::IPC::Host::IpcContextConfig config{};

        // 如果提供了 --main-pid 参数，设置连接模式
        if (vm.count("main-pid"))
        {
            config.main_pid = vm["main-pid"].as<uint32_t>();
            std::string _log_msg = DAS_FMT_NS::format(
                "Host process running in CONNECT mode, main PID: {}",
                config.main_pid);
            DAS_LOG_INFO(_log_msg.c_str());
        }
        else
        {
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

        RegisterLoadPluginHandler(ctx.get());

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

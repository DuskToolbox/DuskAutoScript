// DAS Host Process - IPC Host Entry Point
// B8 Host 进程模型实现

#include <atomic>
#include <boost/program_options.hpp>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HandshakeSerialization.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <das/Utils/fmt.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

// Global IPC context pointer for signal handler
static Das::Core::IPC::Host::IIpcContext* g_ipc_context = nullptr;

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

    static Das::DasPtr<DAS::Core::ForeignInterfaceHost::IForeignLanguageRuntime>
        g_runtime;
}

// 握手完成回调

void HostOnHandshakeComplete(
    Das::Core::IPC::Host::IIpcContext* ctx,
    DasResult                          result,
    void*                              user_data)
{
    (void)user_data;

    if (Das::IsFailed(result))
    {
        std::string msg = DAS_FMT_NS::format(
            "[OnHandshakeComplete] Handshake failed: 0x{:08X}",
            static_cast<uint32_t>(result));
        DAS_LOG_ERROR(msg.c_str());
        ctx->RequestStop();
        return;
    }

    std::string msg =
        DAS_FMT_NS::format("[OnHandshakeComplete] Handshake succeeded");
    DAS_LOG_INFO(msg.c_str());

    // 注册 LOAD_PLUGIN 处理器
    ctx->RegisterCommandHandler(
        static_cast<uint32_t>(Das::Core::IPC::IpcCommandType::LOAD_PLUGIN),
        [ctx](
            const Das::Core::IPC::IPCMessageHeader& header,
            std::span<const uint8_t>                payload,
            Das::Core::IPC::IpcCommandResponse&     response) -> DasResult
        {
            (void)header;

            if (!g_runtime)
            {
                response.error_code = DAS_E_OBJECT_NOT_INIT;
                return DAS_E_OBJECT_NOT_INIT;
            }

            // Deserialize manifest_path from payload
            std::string manifest_path;
            size_t      offset = 0;
            if (!Das::Core::IPC::DeserializeString(
                    payload,
                    offset,
                    manifest_path))
            {
                response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
                return DAS_E_IPC_INVALID_MESSAGE_BODY;
            }

            // Read and parse manifest JSON file
            std::ifstream json_file(manifest_path);
            if (!json_file.is_open())
            {
                std::string msg = DAS_FMT_NS::format(
                    "Failed to open manifest file: {}",
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
                    "Failed to parse manifest JSON: {} - Error: {}",
                    manifest_path,
                    e.what());
                DAS_LOG_ERROR(msg.c_str());
                response.error_code = DAS_E_IPC_PLUGIN_LOAD_FAILED;
                response.response_data.clear();
                return DAS_E_IPC_PLUGIN_LOAD_FAILED;
            }

            // Extract plugin name and extension
            std::string plugin_name;
            std::string plugin_extension;
            // 检查 g_runtime 是否已初始化
            if (!g_runtime)
            {
                std::string msg = DAS_FMT_NS::format(
                    "Foreign language runtime not initialized");
                DAS_LOG_ERROR(msg.c_str());
                response.error_code = DAS_E_IPC_PLUGIN_LOAD_FAILED;
                response.response_data.clear();
                return DAS_E_IPC_PLUGIN_LOAD_FAILED;
            }

            try
            {
                plugin_name = manifest_json["name"].get<std::string>();
                plugin_extension =
                    manifest_json["pluginFilenameExtension"].get<std::string>();
            }
            catch (const nlohmann::json::exception& e)
            {
                std::string msg = DAS_FMT_NS::format(
                    "Failed to extract plugin info from JSON: {}",
                    e.what());
                DAS_LOG_ERROR(msg.c_str());
                response.error_code = DAS_E_IPC_PLUGIN_LOAD_FAILED;
                response.response_data.clear();
                return DAS_E_IPC_PLUGIN_LOAD_FAILED;
            }

            // Build DLL path: {manifest_dir}/{name}.{extension}
            std::filesystem::path manifest_dir =
                std::filesystem::path(manifest_path).parent_path();
            std::filesystem::path dll_path =
                manifest_dir / (plugin_name + "." + plugin_extension);

            std::string msg = DAS_FMT_NS::format(
                "Loading plugin from: {}",
                dll_path.string());
            DAS_LOG_INFO(msg.c_str());

            // Load plugin
            auto result = g_runtime->LoadPlugin(dll_path.string());
            if (!result.has_value())
            {
                msg = DAS_FMT_NS::format(
                    "Failed to load plugin: {}",
                    dll_path.string());
                DAS_LOG_ERROR(msg.c_str());
                response.error_code = DAS_E_IPC_PLUGIN_LOAD_FAILED;
                response.response_data.clear();
                return DAS_E_IPC_PLUGIN_LOAD_FAILED;
            }

            // 注册插件对象到 DistributedObjectManager
            auto                     plugin_ptr = result.value();
            Das::Core::IPC::ObjectId object_id;
            DasResult reg_result = ctx->GetObjectManager().RegisterLocalObject(
                plugin_ptr.Get(),
                object_id);

            if (Das::IsFailed(reg_result))
            {
                std::string msg = DAS_FMT_NS::format(
                    "[LOAD_PLUGIN] Failed to register object: {:#x}",
                    static_cast<uint32_t>(reg_result));
                DAS_LOG_ERROR(msg.c_str());
                response.error_code = reg_result;
                response.response_data.clear();
                return reg_result;
            }

            // 序列化响应：LoadPluginResponsePayload (28 bytes)
            // object_id (8 bytes) + iid (16 bytes) + session_id (2 bytes) +
            // version (2 bytes)
            response.error_code = DAS_S_OK;

            // Write object_id fields (8 bytes total): session_id(2) +
            // generation(2) + local_id(4)
            response.response_data.push_back(object_id.session_id & 0xFF);
            response.response_data.push_back(
                (object_id.session_id >> 8) & 0xFF);
            response.response_data.push_back(object_id.generation & 0xFF);
            response.response_data.push_back(
                (object_id.generation >> 8) & 0xFF);
            response.response_data.push_back(object_id.local_id & 0xFF);
            response.response_data.push_back((object_id.local_id >> 8) & 0xFF);
            response.response_data.push_back(
                (object_id.local_id >> 16) & 0xFF); // byte 2
            response.response_data.push_back(
                (object_id.local_id >> 24) & 0xFF); // byte 3

            // Write iid (16 bytes)
            const auto& iid = DAS_IID_BASE;
            response.response_data.insert(
                response.response_data.end(),
                reinterpret_cast<const uint8_t*>(&iid),
                reinterpret_cast<const uint8_t*>(&iid) + sizeof(DasGuid));

            // Write session_id (2 bytes)
            response.response_data.push_back(object_id.session_id & 0xFF);
            response.response_data.push_back(
                (object_id.session_id >> 8) & 0xFF);

            // Write version (2 bytes) - use 1 as default
            uint16_t version = 1;
            response.response_data.push_back(version & 0xFF);
            response.response_data.push_back((version >> 8) & 0xFF);

            std::string log_msg = DAS_FMT_NS::format(
                "[LOAD_PLUGIN] Plugin loaded, object_id={{session:{}, gen:{}, local:{}}}, response_size={}",
                object_id.session_id,
                object_id.generation,
                object_id.local_id,
                response.response_data.size());
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
            "Show this help message");

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

        // 初始化 Foreign Language Runtime（根据配置自动选择 C++ 或 Python）
        {
            using namespace DAS::Core::ForeignInterfaceHost;
            auto result = CreateForeignLanguageRuntime(
                ForeignLanguageRuntimeFactoryDesc{
                    ForeignInterfaceLanguage::Cpp});
            if (result.has_value())
            {
                g_runtime = std::move(result.value());
                std::string _log_msg = DAS_FMT_NS::format(
                    "Foreign language runtime initialized successfully");
                DAS_LOG_INFO(_log_msg.c_str());
            }
            else
            {
                std::string _log_msg = DAS_FMT_NS::format(
                    "Failed to initialize foreign language runtime");
                DAS_LOG_WARNING(_log_msg.c_str());
            }
        }

        // 创建 IPC Context
        using namespace Das::Core::IPC::Host;

        IpcContextConfig config{};
        IpcContextPtr    ctx{CreateIpcContext(config)};
        if (!ctx)
        {
            DAS_LOG_ERROR("Failed to create IPC context");
            return EXIT_FAILURE;
        }

        g_ipc_context = ctx.get();

        // 设置握手完成回调
        ctx->SetOnHandshakeComplete(HostOnHandshakeComplete, nullptr);

        // 运行 IPC 事件循环
        DasResult result = ctx->Run();

        g_ipc_context = nullptr;

        if (Das::IsFailed(result))
        {
            std::string err_msg = DAS_FMT_NS::format(
                "IPC context run failed: 0x{:08X}",
                static_cast<uint32_t>(result));
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

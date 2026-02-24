// DAS Host Process - IPC Host Entry Point
// B8 Host 进程模型实现

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/Host/HandshakeHandler.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MessageQueueTransport.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/Core/IPC/ObjectId.h>



#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <iostream>
#include <stdexec/execution.hpp>
#include <string>
#include <filesystem>
#include <fstream>
#include <thread>
#include <boost/program_options.hpp>
#ifdef _WIN32
#include <nlohmann/json.hpp>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif




// DAS error check macro (errors are negative, success >= 0)
#define DAS_HOST_FAILED(x) ((x) < 0)

// Global state
static std::atomic<bool>                                 g_running{true};
static Das::Core::IPC::Host::HandshakeHandler            g_handshake_handler;
static Das::Core::IPC::IpcCommandHandler                 g_command_handler;
static std::unique_ptr<Das::Core::IPC::SharedMemoryPool> g_shared_memory;
static Das::Core::IPC::IpcRunLoop                        g_run_loop;
static uint32_t                                          g_host_pid = 0;
static Das::Core::IPC::DistributedObjectManager g_object_manager;

namespace
{
    // 序列化辅助函数
    template <typename T>
    bool
    DeserializeValue(std::span<const uint8_t> buffer, size_t& offset, T& value)
    {
        if (offset + sizeof(T) > buffer.size())
        {
            return false;
        }
        std::memcpy(&value, buffer.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }

    bool DeserializeString(
        std::span<const uint8_t> buffer,
        size_t&                  offset,
        std::string&             str,
        uint16_t                 max_len = 1024)
    {
        uint16_t len = 0;
        if (!DeserializeValue(buffer, offset, len))
        {
            return false;
        }
        if (len > max_len || offset + len > buffer.size())
        {
            return false;
        }
        str.assign(reinterpret_cast<const char*>(buffer.data() + offset), len);
        offset += len;
        return true;
    }

    static Das::DasPtr<DAS::Core::ForeignInterfaceHost::IForeignLanguageRuntime> g_runtime;
}

static uint32_t GetCurrentPid()
{
#ifdef _WIN32
    return static_cast<uint32_t>(_getpid());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

static void SignalHandler(int signal)
{
    (void)signal;
    g_running.store(false);
}

static DasResult InitializeIpcResources()
{
    g_host_pid = GetCurrentPid();

    std::string host_to_plugin_queue =
        Das::Core::IPC::Host::MakeMessageQueueName(g_host_pid, true);
    std::string plugin_to_host_queue =
        Das::Core::IPC::Host::MakeMessageQueueName(g_host_pid, false);
    std::string shm_name =
        Das::Core::IPC::Host::MakeSharedMemoryName(g_host_pid);

    DasResult result = g_run_loop.Initialize();
    if (DAS_HOST_FAILED(result))
    {
        DAS_LOG_ERROR("Failed to initialize IPC run loop");
        return result;
    }

    // Set session ID for command handler
    g_command_handler.SetSessionId(g_host_pid);

    // Initialize distributed object manager
    DasResult obj_mgr_result = g_object_manager.Initialize(g_host_pid);
    if (obj_mgr_result != DAS_S_OK)
    {
        std::string msg = DAS_FMT_NS::format(
            "Failed to initialize distributed object manager: 0x{:08X}",
            obj_mgr_result);
        DAS_LOG_ERROR(msg.c_str());
        return obj_mgr_result;
    }

    // Initialize handshake handler
    DasResult handshake_result = g_handshake_handler.Initialize(g_host_pid);
    if (handshake_result != DAS_S_OK)
    {
        std::string msg = "Failed to initialize handshake handler: "
                          + std::to_string(handshake_result);
        DAS_LOG_ERROR(msg.c_str());
        return handshake_result;
    }

    Das::Core::IPC::IpcTransport* transport = g_run_loop.GetTransport();
    if (!transport)
    {
        DAS_LOG_ERROR("Failed to get transport from run loop");
        return DAS_E_FAIL;
    }

    result = transport->Initialize(
        host_to_plugin_queue,
        plugin_to_host_queue,
        Das::Core::IPC::Host::DEFAULT_MAX_MESSAGE_SIZE,
        Das::Core::IPC::Host::DEFAULT_MAX_MESSAGES);
    if (DAS_HOST_FAILED(result))
    {
        DAS_LOG_ERROR("Failed to initialize IPC transport");
        return result;
    }

    g_shared_memory = std::make_unique<Das::Core::IPC::SharedMemoryPool>();
    result = g_shared_memory->Initialize(
        shm_name,
        Das::Core::IPC::Host::DEFAULT_SHARED_MEMORY_SIZE);
    if (DAS_HOST_FAILED(result))
    {
        DAS_LOG_ERROR("Failed to initialize shared memory pool");
        return result;
    }

    result = transport->SetSharedMemoryPool(g_shared_memory.get());
    if (DAS_HOST_FAILED(result))
    {
        DAS_LOG_ERROR("Failed to set shared memory pool for transport");
        return result;
    }

    DAS_LOG_INFO("IPC resources initialized successfully");
    return DAS_S_OK;
}

static void ShutdownIpcResources()
{
    g_handshake_handler.Shutdown();

    g_run_loop.Shutdown();

    if (g_shared_memory)
    {
        g_shared_memory->Shutdown();
        g_shared_memory.reset();
    }

    DAS_LOG_INFO("IPC resources shutdown complete");
}

static void RunEventLoop(bool verbose)
{
    std::string msg = DAS_FMT_NS::format(
        "Host process entering event loop (PID: {})",
        g_host_pid);
    DAS_LOG_INFO(msg.c_str());

    g_run_loop.SetRequestHandler(
        [](const Das::Core::IPC::IPCMessageHeader& header,
           const uint8_t*                          body,
           size_t                                  body_size) -> DasResult
        {
            std::vector<uint8_t> response_body;
            DasResult            result = DAS_E_FAIL;

            std::cout << "[Host] Received message, type="
                      << (int)header.message_type << std::endl;
            // First try handshake handler
            result = g_handshake_handler
                         .HandleMessage(header, body, body_size, response_body);
            std::cout << "[Host] Handshake result=" << result << std::endl;

            // If handshake handler doesn't handle it, try command handler
            if (result != DAS_S_OK)
            {
                Das::Core::IPC::IpcCommandResponse cmd_response;
                result = g_command_handler.HandleCommand(
                    header,
                    std::span<const uint8_t>(body, body_size),
                    cmd_response);
                std::string _log_msg = DAS_FMT_NS::format(
                    "[Host] CommandHandler result={}",
                    result);
                DAS_LOG_INFO(_log_msg.c_str());

                if (result == DAS_S_OK)
                {
                    // Combine error code and response data
                    response_body.clear();
                    uint32_t       error_code = cmd_response.error_code;
                    const uint8_t* err_ptr =
                        reinterpret_cast<const uint8_t*>(&error_code);
                    response_body.insert(
                        response_body.end(),
                        err_ptr,
                        err_ptr + sizeof(error_code));
                    response_body.insert(
                        response_body.end(),
                        cmd_response.response_data.begin(),
                        cmd_response.response_data.end());
                }
            }

            // Send response back to client
            if (result == DAS_S_OK)
            {
                Das::Core::IPC::IPCMessageHeader response_header = header;
                response_header.message_type =
                    static_cast<uint8_t>(Das::Core::IPC::MessageType::RESPONSE);
                g_run_loop.SendResponse(
                    response_header,
                    response_body.data(),
                    response_body.size());
            }

            return result;
        });

    auto sender = g_run_loop.RunAsync();
    stdexec::sync_wait(std::move(sender));

    while (g_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    g_run_loop.Stop();

    DAS_LOG_INFO("Host process exiting event loop");
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
            std::cout << desc << "\n";
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
                ForeignLanguageRuntimeFactoryDesc{ForeignInterfaceLanguage::Cpp});
            if (result.has_value())
            {
                g_runtime = std::move(result.value());
                std::string _log_msg = DAS_FMT_NS::format(
                    "Foreign language runtime initialized successfully");
                DAS_LOG_INFO(_log_msg.c_str());
            }
            else
            {
                std::string _log_msg =
                    DAS_FMT_NS::format("Failed to initialize foreign language runtime");
                DAS_LOG_WARNING(_log_msg.c_str());
            }
        }

        // Register LOAD_PLUGIN handler
        g_command_handler.RegisterHandler(
            Das::Core::IPC::IpcCommandType::LOAD_PLUGIN,
            [](const Das::Core::IPC::IPCMessageHeader& header,
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
                if (!DeserializeString(payload, offset, manifest_path))
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
                    return DAS_E_IPC_PLUGIN_LOAD_FAILED;
                }

                // Extract plugin name and extension
                std::string plugin_name;
                std::string plugin_extension;
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
                    return DAS_E_IPC_PLUGIN_LOAD_FAILED;
                }

                // Build DLL path: {manifest_dir}/{name}.{extension}
                std::filesystem::path manifest_dir =
                    std::filesystem::path(manifest_path).parent_path();
                std::filesystem::path dll_path =
                    manifest_dir / (plugin_name + "." + plugin_extension);

                std::string msg = DAS_FMT_NS::format(
                    "Loading plugin from: {}", dll_path.string());
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
                    return DAS_E_IPC_PLUGIN_LOAD_FAILED;
                }

                // 注册插件对象到 DistributedObjectManager
                auto plugin_ptr = result.value();
                Das::Core::IPC::ObjectId object_id;
                DasResult reg_result = g_object_manager.RegisterLocalObject(
                    plugin_ptr.Get(),
                    object_id);

                if (DAS::IsFailed(reg_result))
                {
                    std::string msg = DAS_FMT_NS::format(
                        "[LOAD_PLUGIN] Failed to register object: {:#x}",
                        static_cast<uint32_t>(reg_result));
                    DAS_LOG_ERROR(msg.c_str());
                    response.error_code = reg_result;
                    return reg_result;
                }

                // 序列化响应：编码 object_id 并写入 response_data
                uint64_t encoded_id = Das::Core::IPC::EncodeObjectId(object_id);

                response.error_code = DAS_S_OK;
                response.response_data.resize(8);
                std::memcpy(response.response_data.data(), &encoded_id, 8);

                std::string log_msg = DAS_FMT_NS::format(
                    "[LOAD_PLUGIN] Plugin loaded, object_id={:#x}",
                    encoded_id);
                DAS_LOG_INFO(log_msg.c_str());

                return DAS_S_OK;
            });

        DasResult result = InitializeIpcResources();
        if (DAS_HOST_FAILED(result))
        {
            std::string err_msg = DAS_FMT_NS::format(
                "Failed to initialize IPC resources: 0x{:08X}",
                result);
            DAS_LOG_ERROR(err_msg.c_str());
            return EXIT_FAILURE;
        }

        RunEventLoop(verbose);

        ShutdownIpcResources();

        DAS_LOG_INFO("DAS Host Process shutdown complete");
        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}

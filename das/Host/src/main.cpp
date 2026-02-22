// DAS Host Process - IPC Host Entry Point
// B8 Host 进程模型实现

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/Host/HandshakeHandler.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MessageQueueTransport.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/DasApi.h>
#include <das/Host/HostConfig.h>
#include <das/Utils/fmt.h>
#include <iostream>
#include <stdexec/execution.hpp>
#include <string>
#include <thread>

#include <boost/program_options.hpp>

#ifdef _WIN32
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
        Das::Host::MakeMessageQueueName(g_host_pid, true);
    std::string plugin_to_host_queue =
        Das::Host::MakeMessageQueueName(g_host_pid, false);
    std::string shm_name = Das::Host::MakeSharedMemoryName(g_host_pid);

    DasResult result = g_run_loop.Initialize();
    if (DAS_HOST_FAILED(result))
    {
        DAS_LOG_ERROR("Failed to initialize IPC run loop");
        return result;
    }

    // Set session ID for command handler
    g_command_handler.SetSessionId(g_host_pid);

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
        Das::Host::DEFAULT_MAX_MESSAGE_SIZE,
        Das::Host::DEFAULT_MAX_MESSAGES);
    if (DAS_HOST_FAILED(result))
    {
        DAS_LOG_ERROR("Failed to initialize IPC transport");
        return result;
    }

    g_shared_memory = std::make_unique<Das::Core::IPC::SharedMemoryPool>();
    result = g_shared_memory->Initialize(
        shm_name,
        Das::Host::DEFAULT_SHARED_MEMORY_SIZE);
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

            std::cout << "[Host] Received message, type=" << (int)header.message_type << std::endl;
            // First try handshake handler
            result = g_handshake_handler
                         .HandleMessage(header, body, body_size, response_body);
            std::cout << "[Host] Handshake result=" << result.code << std::endl;

            // If handshake handler doesn't handle it, try command handler
            if (result != DAS_S_OK)
            {
                Das::Core::IPC::IpcCommandResponse cmd_response;
                result = g_command_handler.HandleCommand(
                    header,
                    std::span<const uint8_t>(body, body_size),
                    cmd_response);

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

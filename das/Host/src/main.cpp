// DAS Host Process - IPC Host Entry Point
// B8 Host 进程模型实现

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/MessageQueueTransport.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/DasApi.h>
#include <das/Host/HostConfig.h>
#include <string>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace Das::Core::IPC;
using namespace Das::Host;

// DAS error check macro (errors are negative, success >= 0)
#define DAS_HOST_FAILED(x) ((x) < 0)

// Global state
static std::atomic<bool>                 g_running{true};
static std::atomic<uint16_t>             g_next_session_id{MIN_SESSION_ID};
static std::unique_ptr<IpcTransport>     g_transport;
static std::unique_ptr<SharedMemoryPool> g_shared_memory;
static uint32_t                          g_host_pid = 0;

// B8.1: session_id 分配（全局原子计数器）
static uint16_t AllocateSessionId()
{
    uint16_t id = g_next_session_id.fetch_add(1);
    if (id > MAX_SESSION_ID)
    {
        g_next_session_id.store(MIN_SESSION_ID);
        id = g_next_session_id.fetch_add(1);
    }
    return id;
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

static void PrintUsage(const char* program_name)
{
    std::printf("DAS Host Process - IPC Resource Owner\n\n");
    std::printf("Usage: %s [options]\n\n", program_name);
    std::printf("Options:\n");
    std::printf("  --verbose    Enable verbose logging\n");
    std::printf("  --help       Show this help message\n");
}

static bool ParseCommandLine(int argc, char* argv[], bool& verbose)
{
    verbose = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--verbose")
        {
            verbose = true;
        }
        else if (arg == "--help")
        {
            PrintUsage(argv[0]);
            return false;
        }
        else
        {
            std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
            PrintUsage(argv[0]);
            return false;
        }
    }
    return true;
}

static DasResult InitializeIpcResources()
{
    g_host_pid = GetCurrentPid();

    std::string host_to_plugin_queue = MakeMessageQueueName(g_host_pid, true);
    std::string plugin_to_host_queue = MakeMessageQueueName(g_host_pid, false);
    std::string shm_name = MakeSharedMemoryName(g_host_pid);

    g_transport = std::make_unique<IpcTransport>();
    DasResult result = g_transport->Initialize(
        host_to_plugin_queue,
        plugin_to_host_queue,
        DEFAULT_MAX_MESSAGE_SIZE,
        DEFAULT_MAX_MESSAGES);
    if (DAS_HOST_FAILED(result))
    {
        DAS_LOG_ERROR("Failed to initialize IPC transport");
        return result;
    }

    g_shared_memory = std::make_unique<SharedMemoryPool>();
    result = g_shared_memory->Initialize(shm_name, DEFAULT_SHARED_MEMORY_SIZE);
    if (DAS_HOST_FAILED(result))
    {
        DAS_LOG_ERROR("Failed to initialize shared memory pool");
        return result;
    }

    result = g_transport->SetSharedMemoryPool(g_shared_memory.get());
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
    if (g_transport)
    {
        g_transport->Shutdown();
        g_transport.reset();
    }

    if (g_shared_memory)
    {
        g_shared_memory->Shutdown();
        g_shared_memory.reset();
    }

    DAS_LOG_INFO("IPC resources shutdown complete");
}

// Event loop placeholder
static void RunEventLoop(bool verbose)
{
    char buffer[256];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "Host process entering event loop (PID: %u)",
        g_host_pid);
    DAS_LOG_INFO(buffer);

    while (g_running.load())
    {
        IPCMessageHeader     header;
        std::vector<uint8_t> body;

        DasResult result =
            g_transport->Receive(header, body, HEARTBEAT_INTERVAL_MS);

        if (result == DAS_E_IPC_TIMEOUT)
        {
            continue;
        }

        if (DAS_HOST_FAILED(result))
        {
            if (verbose)
            {
                std::snprintf(
                    buffer,
                    sizeof(buffer),
                    "Receive failed: 0x%08X",
                    result);
                DAS_LOG_WARNING(buffer);
            }
            continue;
        }

        if (verbose)
        {
            std::snprintf(
                buffer,
                sizeof(buffer),
                "Received message: call_id=%llu, type=%d",
                static_cast<unsigned long long>(header.call_id),
                static_cast<int>(header.message_type));
            DAS_LOG_INFO(buffer);
        }

        // TODO: Implement message dispatch (Task 7.2+)
        // 1. Parse message type
        // 2. Dispatch to appropriate handler
        // 3. Send response
    }

    DAS_LOG_INFO("Host process exiting event loop");
}

int main(int argc, char* argv[])
{
    bool verbose = false;

    if (!ParseCommandLine(argc, argv, verbose))
    {
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
#ifdef SIGBREAK
    std::signal(SIGBREAK, SignalHandler);
#endif

    DAS_LOG_INFO("DAS Host Process starting...");

    DasResult result = InitializeIpcResources();
    if (DAS_HOST_FAILED(result))
    {
        char err_buf[128];
        std::snprintf(
            err_buf,
            sizeof(err_buf),
            "Failed to initialize IPC resources: 0x%08X",
            result);
        DAS_LOG_ERROR(err_buf);
        return EXIT_FAILURE;
    }

    uint16_t test_session_id = AllocateSessionId();
    char     session_buf[128];
    std::snprintf(
        session_buf,
        sizeof(session_buf),
        "Test session_id allocated: %u",
        test_session_id);
    DAS_LOG_INFO(session_buf);

    RunEventLoop(verbose);

    ShutdownIpcResources();

    DAS_LOG_INFO("DAS Host Process shutdown complete");
    return EXIT_SUCCESS;
}

// DAS Host Process - IPC Host Entry Point
// B8 Host 进程模型实现

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MessageQueueTransport.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/DasApi.h>
#include <das/Host/HostConfig.h>
#include <stdexec/execution.hpp>
#include <string>
#include <thread>

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
static std::unique_ptr<SharedMemoryPool> g_shared_memory;
static IpcRunLoop                        g_run_loop;
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

    DasResult result = g_run_loop.Initialize();
    if (DAS_HOST_FAILED(result))
    {
        DAS_LOG_ERROR("Failed to initialize IPC run loop");
        return result;
    }

    IpcTransport* transport = g_run_loop.GetTransport();
    if (!transport)
    {
        DAS_LOG_ERROR("Failed to get transport from run loop");
        return DAS_E_FAIL;
    }

    result = transport->Initialize(
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
    char buffer[256];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "Host process entering event loop (PID: %u)",
        g_host_pid);
    DAS_LOG_INFO(buffer);

    g_run_loop.SetRequestHandler(
        [verbose](
            const IPCMessageHeader& header,
            const uint8_t*          body,
            size_t                  body_size) -> DasResult
        {
            (void)body;
            (void)body_size;

            if (verbose)
            {
                char buf[256];
                std::snprintf(
                    buf,
                    sizeof(buf),
                    "Received message: call_id=%llu, type=%d",
                    static_cast<unsigned long long>(header.call_id),
                    static_cast<int>(header.message_type));
                DAS_LOG_INFO(buf);
            }

            return DAS_S_OK;
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

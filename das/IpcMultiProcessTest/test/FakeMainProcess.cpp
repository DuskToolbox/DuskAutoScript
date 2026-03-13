/**
 * @file FakeMainProcess.cpp
 * @brief 跨进程就绪信号和假主进程实现
 */

#include "FakeMainProcess.h"
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Utils/fmt.h>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace FakeMainProcess {

std::string GenerateUniqueSignalName()
{
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

#ifdef _WIN32
    uint32_t pid = GetCurrentProcessId();
#else
    uint32_t pid = getpid();
#endif

    return DAS_FMT_NS::format("das_fake_main_{}_{}", pid, ms);
}

FakeMainReadySignal::FakeMainReadySignal(const std::string& signal_name)
    : signal_name_(signal_name)
{
    // 先移除旧的互斥锁（防止上次测试异常退出留下残留）
    try
    {
        boost::interprocess::named_mutex::remove(signal_name.c_str());
    }
    catch (...)
    {
    }

    mutex_ = std::make_unique<boost::interprocess::named_mutex>(
        boost::interprocess::open_or_create, signal_name.c_str());
}

FakeMainReadySignal::~FakeMainReadySignal()
{
    try
    {
        boost::interprocess::named_mutex::remove(signal_name_.c_str());
    }
    catch (...)
    {
    }
}

void FakeMainReadySignal::NotifyReady()
{
    ready_ = true;
    // 锁的释放会通知等待方
}

bool FakeMainReadySignal::WaitForReady(std::chrono::milliseconds timeout_ms)
{
    auto deadline = std::chrono::steady_clock::now() + timeout_ms;

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (mutex_->try_lock())
        {
            mutex_->unlock();
            if (ready_)
            {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void FakeMainReadySignal::Cleanup(const std::string& signal_name)
{
    try
    {
        boost::interprocess::named_mutex::remove(signal_name.c_str());
    }
    catch (...)
    {
    }
}

int RunFakeMainProcessMode(const std::string& signal_name)
{
    DAS_LOG_INFO(DAS_FMT_NS::format(
        "[FakeMain] Starting with signal: {}",
        signal_name).c_str());

    // 1. 创建 IPC 上下文（作为主进程）
    auto ctx = DAS::Core::IPC::MainProcess::CreateIpcContextShared();
    if (!ctx)
    {
        DAS_LOG_ERROR("[FakeMain] Failed to create IPC context");
        return 1;
    }

    DAS_LOG_INFO("[FakeMain] IPC context created, waiting for Host connection...");

    // 2. 短暂等待后通知就绪（让 IPC 上下文有足够时间初始化）
    // 由于没有 Host 连接回调，使用简单的延迟来模拟就绪
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 通知测试主进程 IPC 上下文已就绪
    {
        FakeMainReadySignal signal(signal_name);
        signal.NotifyReady();
        DAS_LOG_INFO("[FakeMain] Ready signal sent");
    }

    // 3. 运行事件循环（阻塞，等待 Host 连接）
    ctx->Run();

    DAS_LOG_INFO("[FakeMain] Exiting");
    return 0;
}

} // namespace FakeMainProcess

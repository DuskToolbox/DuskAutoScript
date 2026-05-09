#ifndef DAS_CORE_IPC_IPC_RUNTIME_STATE_H
#define DAS_CORE_IPC_IPC_RUNTIME_STATE_H

#include <atomic>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

class IpcRuntimeState
{
public:
    void BeginShutdown() noexcept
    {
        shutting_down_.store(true, std::memory_order_release);
    }

    [[nodiscard]]
    bool IsShuttingDown() const noexcept
    {
        return shutting_down_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> shutting_down_{false};
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RUNTIME_STATE_H

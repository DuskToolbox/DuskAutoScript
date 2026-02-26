#ifndef DAS_CORE_IPC_IPC_TIMEOUT_CONFIG_H
#define DAS_CORE_IPC_IPC_TIMEOUT_CONFIG_H

#include <chrono>
#include <cstdint>
#include <das/DasConfig.h>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

namespace IpcTimeoutConfig
{

    /// 原子地获取当前超时值并重置为 0（无限）
    /// 供 IpcRunLoop 内部使用，每次 IPC 操作前调用
    /// @return 当前超时值（毫秒），0 表示无限超时
    std::chrono::milliseconds FetchAndResetTimeout();

    /// 设置当前线程的超时值
    void SetTimeout(uint32_t timeout_ms);

    /// 获取当前线程的超时值（不清零）
    uint32_t GetTimeout();

} // namespace IpcTimeoutConfig

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_TIMEOUT_CONFIG_H

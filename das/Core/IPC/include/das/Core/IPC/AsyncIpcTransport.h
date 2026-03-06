#ifndef DAS_CORE_IPC_ASYNC_IPC_TRANSPORT_H
#define DAS_CORE_IPC_ASYNC_IPC_TRANSPORT_H

#include <cstdint>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <memory>
#include <stdexec/execution.hpp>
#include <string>
#include <utility>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

class SharedMemoryPool;

/**
 * @brief 异步 IPC 传输结果
 * @details 包含消息头和消息体的配对
 */
using AsyncIpcMessage =
    std::pair<ValidatedIPCMessageHeader, std::vector<uint8_t>>;

/**
 * @brief 大消息阈值（64KB）
 */
inline constexpr size_t LARGE_MESSAGE_THRESHOLD = 65536;

// === 编译期平台选择 ===
// 具体实现由平台特定文件提供：
// - Win32AsyncIpcTransport (Windows)
// - UnixAsyncIpcTransport (Linux/macOS)

#ifdef _WIN32
class Win32AsyncIpcTransport;
using DefaultAsyncIpcTransport = Win32AsyncIpcTransport;
#else
class UnixAsyncIpcTransport;
using DefaultAsyncIpcTransport = UnixAsyncIpcTransport;
#endif

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_ASYNC_IPC_TRANSPORT_H

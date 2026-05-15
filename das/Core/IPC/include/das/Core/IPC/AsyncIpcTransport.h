#ifndef DAS_CORE_IPC_ASYNC_IPC_TRANSPORT_H
#define DAS_CORE_IPC_ASYNC_IPC_TRANSPORT_H

#include <cstdint>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <memory>
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
 *
 * 超过此阈值的消息将通过共享内存传输而非管道，由 IDL 生成的 Stub 代码使用。
 */
inline constexpr size_t LARGE_MESSAGE_THRESHOLD = 65536;

// === 编译期平台选择 ===
// 具体实现由平台特定文件提供：
// - Win32AsyncIpcTransport (Windows, 默认)
// - UnixAsyncIpcTransport (Linux/macOS；Windows 上可通过 AfUnixAvailable() 运行时检测)

#ifdef DAS_WINDOWS
class Win32AsyncIpcTransport;
class UnixAsyncIpcTransport; // 在 Windows 上也可用（需 Win10
                             // 1803+，运行时检测）
using DefaultAsyncIpcTransport = Win32AsyncIpcTransport;
#else
class UnixAsyncIpcTransport;
using DefaultAsyncIpcTransport = UnixAsyncIpcTransport;
#endif

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_ASYNC_IPC_TRANSPORT_H

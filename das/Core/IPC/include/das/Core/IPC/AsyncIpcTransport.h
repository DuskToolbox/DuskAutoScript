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

/**
 * @brief 非拥有发送段（scatter-gather 发送用）
 * @details data 指向的备存储不归本结构所有：调用方须保证其存活至
 *          async_write 完成——把备存储的持有者（如 pending 帧容器）
 *          按值 move 进协程帧，或协程被 co_await 期间不释放。
 *          生命周期契约违反 = 悬空读（ASAN 高发点）。
 */
struct SendSegment
{
    const uint8_t* data = nullptr;
    size_t         size = 0;
};

// SendScatter 契约（不存在 AsyncIpcTransport 基类——四个实现点以同签名
// 约束，AnyTransport 的 std::visit 使漏实现 = 编译错误）：
//
//   boost::asio::awaitable<DasResult> SendScatter(
//       const ValidatedIPCMessageHeader& header,
//       std::vector<SendSegment>&&       segments);
//
// 线上字节 = header 与各段顺序逐段拼接（与拼接后走单段 Send 完全一致，
// 新旧互通、接收端零改动）。additive 重载：现有 Send 签名不动。
// header 与段容器由调用方保证存活至本协程完成（协程被 co_await，或
// 段列表已被实现 move 进协程帧后调用方不再释放）。

// 具体实现由平台特定文件提供：
// - Win32AsyncIpcTransport (Windows Named Pipe)
// - UnixAsyncIpcTransport (Linux/macOS；Windows 上可通过 AfUnixAvailable()
//   运行时检测)

#ifdef DAS_WINDOWS
class Win32AsyncIpcTransport;
class UnixAsyncIpcTransport; // 在 Windows 上也可用（需 Win10
                             // 1803+，运行时检测）
#else
class UnixAsyncIpcTransport;
#endif

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_ASYNC_IPC_TRANSPORT_H

#ifndef DAS_CORE_IPC_IPC_RESPONSE_SENDER_H
#define DAS_CORE_IPC_IPC_RESPONSE_SENDER_H

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <vector>

#include <das/Core/IPC/Config.h>

// Forward declarations for platform-specific transport
#ifdef _WIN32
namespace Das::Core::IPC
{
class Win32AsyncIpcTransport;
}
#else
namespace Das::Core::IPC
{
class UnixAsyncIpcTransport;
}
#endif

DAS_CORE_IPC_NS_BEGIN

class IpcRunLoop;

/**
 * @brief IPC 响应发送器
 *
 * 提供响应发送功能的轻量级包装器。
 * IMessageHandler 通过此接口发送响应。
 *
 * 支持两种模式：
 * 1. 使用 IpcRunLoop（旧模式，已废弃）
 * 2. 直接使用 transport（新模式，推荐）
 */
class IpcResponseSender
{
public:
    /**
     * @brief 构造函数（旧模式，已废弃）
     * @param run_loop IPC 运行循环（必须有效）
     * @deprecated 使用带 transport 参数的构造函数代替
     */
    explicit IpcResponseSender(IpcRunLoop& run_loop);

#ifdef _WIN32
    /**
     * @brief 构造函数（新模式，推荐）
     * @param transport Win32 异步 IPC 传输层（必须有效）
     */
    explicit IpcResponseSender(Win32AsyncIpcTransport& transport);
#else
    /**
     * @brief 构造函数（新模式，推荐）
     * @param transport Unix 异步 IPC 传输层（必须有效）
     */
    explicit IpcResponseSender(UnixAsyncIpcTransport& transport);
#endif

    /**
     * @brief 发送响应（协程版本）
     * @param header 响应消息头
     * @param body 响应消息体
     * @return boost::asio::awaitable<DasResult> 协程结果
     */
    boost::asio::awaitable<DasResult> SendResponse(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body);

private:
    IpcRunLoop* run_loop_ = nullptr;
#ifdef _WIN32
    Win32AsyncIpcTransport* transport_ = nullptr;
#else
    UnixAsyncIpcTransport* transport_ = nullptr;
#endif
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RESPONSE_SENDER_H

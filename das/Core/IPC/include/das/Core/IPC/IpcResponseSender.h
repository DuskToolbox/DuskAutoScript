#ifndef DAS_CORE_IPC_IPC_RESPONSE_SENDER_H
#define DAS_CORE_IPC_IPC_RESPONSE_SENDER_H

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <vector>

#include <das/Core/IPC/Config.h>
#include <das/DasConfig.h>

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

/**
 * @brief IPC 响应发送器
 *
 * 提供响应发送功能的轻量级包装器。
 * IMessageHandler 通过此接口发送响应。
 *
 * 使用 transport 直接发送响应。
 */
class IpcResponseSender
{
public:
#ifdef _WIN32
    /**
     * @brief 构造函数
     * @param transport Win32 异步 IPC 传输层（必须有效）
     */
    explicit IpcResponseSender(Win32AsyncIpcTransport& DAS_LIFETIMEBOUND transport);
#else
    /**
     * @brief 构造函数
     * @param transport Unix 异步 IPC 传输层（必须有效）
     */
    explicit IpcResponseSender(UnixAsyncIpcTransport& DAS_LIFETIMEBOUND transport);
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
#ifdef _WIN32
    Win32AsyncIpcTransport* transport_ = nullptr;
#else
    UnixAsyncIpcTransport* transport_ = nullptr;
#endif
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RESPONSE_SENDER_H

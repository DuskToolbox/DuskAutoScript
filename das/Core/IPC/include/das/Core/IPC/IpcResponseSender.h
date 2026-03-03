#ifndef DAS_CORE_IPC_IPC_RESPONSE_SENDER_H
#define DAS_CORE_IPC_IPC_RESPONSE_SENDER_H

#include <cstdint>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN
class IpcRunLoop;

/**
 * @brief IPC 响应发送器
 *
 * 提供响应发送功能的轻量级包装器。
 * IMessageHandler 通过此接口发送响应。
 */
class IpcResponseSender
{
public:
    /**
     * @brief 构造函数
     * @param run_loop IPC 运行循环（必须有效）
     */
    explicit IpcResponseSender(IpcRunLoop& run_loop);

    /**
     * @brief 发送响应
     * @param header 响应消息头
     * @param body 响应消息体
     * @return DasResult 成功返回 DAS_S_OK
     */
    DasResult SendResponse(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body);

private:
    IpcRunLoop& run_loop_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RESPONSE_SENDER_H

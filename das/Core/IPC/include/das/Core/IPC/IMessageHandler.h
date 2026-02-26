#pragma once

#include <cstdint>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/DasApi.h>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

// 前向声明
class IpcResponseSender;

/**
 * @brief 消息处理器接口
 *
 * 所有 IPC 消息处理器必须实现此接口。
 * 通过 RegisterHandler() 注册到 IpcRunLoop。
 */
class DAS_API IMessageHandler
{
public:
    virtual ~IMessageHandler() = default;

    /**
     * @brief 获取处理器负责的接口 ID
     * @return 接口 ID（用于消息分发）
     */
    [[nodiscard]]
    virtual uint32_t GetInterfaceId() const = 0;

    /**
     * @brief 处理 IPC 消息
     * @param header 消息头
     * @param body 消息体
     * @param sender 响应发送器（用于发送响应）
     * @return 处理结果
     */
    virtual DasResult HandleMessage(
        const IPCMessageHeader&     header,
        const std::vector<uint8_t>& body,
        IpcResponseSender&          sender) = 0;
};

DAS_CORE_IPC_NS_END

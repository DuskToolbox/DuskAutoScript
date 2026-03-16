#pragma once

#include <cstdint>
#include <das/DasApi.h>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

// 前向声明
class IpcResponseSender;
class ValidatedIPCMessageHeader;
class DistributedObjectManager;

/**
 * @brief 消息处理器接口
 *
 * 所有 IPC 消息处理器必须实现此接口。
 * 通过 RegisterHandler() 注册到 IpcRunLoop。
 */
class IMessageHandler
{
public:
    virtual ~IMessageHandler() = default;

    /**
     * @brief 增加引用计数
     * @return 新的引用计数
     */
    [[nodiscard]]
    virtual uint32_t AddRef() = 0;

    /**
     * @brief 减少引用计数
     * @return 新的引用计数
     */
    [[nodiscard]]
    virtual uint32_t Release() = 0;

    /**
     * @brief 获取处理器负责的接口 ID
     * @return 接口 ID（用于消息分发）
     */
    [[nodiscard]]
    virtual uint32_t GetInterfaceId() const = 0;

    /**
     * @brief 处理 IPC 消息（同步版本）
     * @param header 已验证的消息头
     * @param body 消息体
     * @param sender 响应发送器（用于发送响应）
     * @param object_manager 分布式对象管理器（用于查找 impl 指针）
     * @return DasResult 处理结果
     */
    virtual DasResult HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        DistributedObjectManager&        object_manager) = 0;
};

DAS_CORE_IPC_NS_END

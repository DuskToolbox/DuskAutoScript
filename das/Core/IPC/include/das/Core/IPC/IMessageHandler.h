#pragma once

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <das/DasApi.h>
#include <memory>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

// 前向声明
class IpcResponseSender;
class ValidatedIPCMessageHeader;
class DistributedObjectManager;
class IpcRunLoop;
class BusinessThread;

/**
 * @brief 控制平面 handler 上下文
 *
 * 轻量级上下文，仅包含控制平面 handler 所需的运行时引用。
 * 不包含 DistributedObjectManager，因为控制平面 handler 不使用对象管理。
 */
struct ControlHandlerContext
{
    ControlHandlerContext(IpcRunLoop& rl, const ValidatedIPCMessageHeader& hdr)
        : run_loop(rl), header(hdr)
    {
    }

    IpcRunLoop&                      run_loop;
    const ValidatedIPCMessageHeader& header;
};

/**
 * @brief Stub 上下文结构体
 *
 * 打包业务 handler 所需的运行时引用，避免虚函数签名参数膨胀。
 * 新增字段只需修改此结构体，无需修改虚函数签名。
 */
struct StubContext
{
    StubContext(
        DistributedObjectManager&        obj_mgr,
        IpcRunLoop&                      rl,
        std::weak_ptr<BusinessThread>    bt,
        const ValidatedIPCMessageHeader& hdr)
        : object_manager(obj_mgr), run_loop(rl), business_thread(bt),
          header(hdr)
    {
    }

    DistributedObjectManager&        object_manager;
    IpcRunLoop&                      run_loop;
    std::weak_ptr<BusinessThread>    business_thread;
    const ValidatedIPCMessageHeader& header;
    uint16_t                         response_flags =
        0; ///< 由 Handle* 方法设置，传递给响应头 flags 字段
};

/**
 * @brief 控制平面消息处理器接口
 *
 * 用于控制平面 handler（如 HandshakeHandler），不需要
 * DistributedObjectManager。 使用 ControlHandlerContext 替代
 * StubContext，消除对 DistributedObjectManager 的依赖。
 */
struct IControlHandler
{
    virtual ~IControlHandler() = default;

    [[nodiscard]]
    virtual uint32_t AddRef() = 0;
    [[nodiscard]]
    virtual uint32_t Release() = 0;
    [[nodiscard]]
    virtual uint32_t GetInterfaceId() const = 0;

    virtual boost::asio::awaitable<DasResult> HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        ControlHandlerContext&           ctx) = 0;
};

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
     * @param ctx Stub 上下文（包含 object_manager、run_loop、business_thread）
     * @return DasResult 处理结果
     */
    virtual DasResult HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        StubContext&                     ctx) = 0;
};

/**
 * @brief 可等待消息处理器接口（协程版本）
 *
 * 控制平面 handler（如 HandshakeHandler）使用此接口，
 * 通过协程异步发送响应。
 *
 * 注意：不继承 IMessageHandler，因为 HandleMessage 返回类型不同。
 * 实现类需要同时实现两个接口的引用计数方法。
 */
class IAwaitableMessageHandler
{
public:
    virtual ~IAwaitableMessageHandler() = default;

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
     * @brief 处理 IPC 消息（协程版本）
     *
     * @param header 已验证的消息头
     * @param body 消息体
     * @param sender 响应发送器（包含 transport）
     * @param ctx Stub 上下文（包含 object_manager、run_loop、business_thread）
     * @return boost::asio::awaitable<DasResult> 协程结果
     */
    virtual boost::asio::awaitable<DasResult> HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        StubContext&                     ctx) = 0;
};

DAS_CORE_IPC_NS_END

#ifndef DAS_CORE_IPC_MESSAGE_HANDLER_REF_H
#define DAS_CORE_IPC_MESSAGE_HANDLER_REF_H

#include <boost/asio/awaitable.hpp>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/DasApi.h>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN
/**
 * @brief 消息处理器引用适配器
 *
 * 包装 IMessageHandler 的原始指针，不转移所有权。
 * 用于将已存在的处理器注册到 IpcRunLoop 而不影响其生命周期。
 *
 * @note 适配器不拥有处理器的所有权，调用者必须确保被包装的处理器
 *       在适配器生命周期内保持有效。
 */
class MessageHandlerRef : public IMessageHandler
{
public:
    /**
     * @brief 构造函数
     * @param handler 被包装的处理器（必须非空且在适配器生命周期内有效）
     */
    explicit MessageHandlerRef(IMessageHandler* handler) : handler_(handler) {}

    ~MessageHandlerRef() override = default;

    // 禁用拷贝
    MessageHandlerRef(const MessageHandlerRef&) = delete;
    MessageHandlerRef& operator=(const MessageHandlerRef&) = delete;

    /// 增加引用计数（委托给被包装的处理器）
    [[nodiscard]]
    uint32_t AddRef() override
    {
        return handler_ ? handler_->AddRef() : 0;
    }

    /// 减少引用计数（委托给被包装的处理器）
    [[nodiscard]]
    uint32_t Release() override
    {
        return handler_ ? handler_->Release() : 0;
    }

    [[nodiscard]]
    uint32_t GetInterfaceId() const override
    {
        return handler_ ? handler_->GetInterfaceId() : 0;
    }

    boost::asio::awaitable<DasResult> HandleMessage(
        const IPCMessageHeader&     header,
        const std::vector<uint8_t>& body,
        IpcResponseSender&          sender) override
    {
        if (!handler_)
        {
            co_return DAS_E_IPC_INVALID_ARGUMENT;
        }
        co_return co_await handler_->HandleMessage(header, body, sender);
    }

private:
    IMessageHandler* handler_; ///< 非拥有的处理器指针
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_MESSAGE_HANDLER_REF_H

#ifndef DAS_CORE_IPC_INTERNAL_CALLBACK_HANDLER_H
#define DAS_CORE_IPC_INTERNAL_CALLBACK_HANDLER_H

#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncCallback.h>
#include <das/Utils/CommonUtils.hpp>

DAS_CORE_IPC_NS_BEGIN

/// @brief 内部回调命令枚举（从 1 开始，避免与 IpcCommandType 冲突）
/// @note 值空间独立于 IpcCommandType，通过 header_flags = BUSINESS_CONTROL
///       + interface_id 路由到本 handler
enum class InternalBusinessCommand : uint32_t
{
    ASYNC_CALLBACK = 1,
};

/**
 * @brief 内部回调消息处理器
 *
 * 处理 PostToBusinessThread 投递的 IDasAsyncCallback 消息。
 * 从 body 中提取 IDasAsyncCallback* 指针，用 DasPtr::Attach 管理生命周期，
 * 调用 Do() 后返回 DAS_S_OK（fire-and-forget）。
 */
class InternalCallbackHandler : public IMessageHandler
{
public:
    InternalCallbackHandler() = default;
    ~InternalCallbackHandler() override = default;

    DAS_UTILS_IDASBASE_AUTO_IMPL(InternalCallbackHandler)

    [[nodiscard]]
    uint32_t GetInterfaceId() const noexcept override
    {
        return static_cast<uint32_t>(InternalBusinessCommand::ASYNC_CALLBACK);
    }

    DasResult HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        StubContext&                     ctx) override;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_INTERNAL_CALLBACK_HANDLER_H

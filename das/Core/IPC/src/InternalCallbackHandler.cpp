#include <cstring>
#include <das/Core/IPC/InternalCallbackHandler.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/Logger/Logger.h>

DAS_CORE_IPC_NS_BEGIN

DasResult InternalCallbackHandler::HandleMessage(
    const ValidatedIPCMessageHeader& /*header*/,
    const std::vector<uint8_t>& body,
    IpcResponseSender& /*sender*/,
    StubContext& /*ctx*/)
{
    // body 应包含恰好一个 IDasAsyncCallback* 大小的数据
    if (body.size() != sizeof(IDasAsyncCallback*))
    {
        DAS_CORE_LOG_ERROR(
            "InternalCallbackHandler: invalid body size={}, expected {}",
            body.size(),
            sizeof(IDasAsyncCallback*));
        return DAS_E_INVALID_ARGUMENT;
    }

    // 从 body 提取原始指针
    IDasAsyncCallback* raw_cb = nullptr;
    std::memcpy(&raw_cb, body.data(), sizeof(IDasAsyncCallback*));

    if (!raw_cb)
    {
        DAS_CORE_LOG_ERROR(
            "InternalCallbackHandler: null callback pointer in body");
        return DAS_E_INVALID_POINTER;
    }

    // DasPtr::Attach 接管已有引用（不 AddRef），析构时自动 Release
    DasPtr<IDasAsyncCallback> cb = DasPtr<IDasAsyncCallback>::Attach(raw_cb);

    cb->Do();

    // fire-and-forget: 不发送 response
    return DAS_S_OK;
}

DAS_CORE_IPC_NS_END

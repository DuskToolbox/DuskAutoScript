#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcRunLoop.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        DasResult IPCProxyBase::SendRequest(
            uint16_t              method_id,
            const uint8_t*        body,
            size_t                body_size,
            std::vector<uint8_t>& response_body)
        {
            if (!run_loop_)
            {
                return DAS_E_FAIL;
            }

            // 分配调用 ID
            uint64_t call_id = AllocateCallId();

            // 填充消息头
            IPCMessageHeader header;
            FillMessageHeader(
                header,
                method_id,
                call_id,
                MessageType::REQUEST,
                body_size);

            // 调用 IpcRunLoop 发送请求
            return run_loop_
                ->SendRequest(header, body, body_size, response_body);
        }

        DasResult IPCProxyBase::SendRequestNoResponse(
            uint16_t       method_id,
            const uint8_t* body,
            size_t         body_size)
        {
            if (!run_loop_)
            {
                return DAS_E_FAIL;
            }

            // 分配调用 ID
            uint64_t call_id = AllocateCallId();

            // 填充消息头（无响应）
            IPCMessageHeader header;
            FillMessageHeader(
                header,
                method_id,
                call_id,
                MessageType::EVENT,
                body_size);

            // 调用 IpcRunLoop 发送请求
            return run_loop_->SendEvent(header, body, body_size);
        }
    }
}
DAS_NS_END

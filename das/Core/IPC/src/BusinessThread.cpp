#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/Core/Logger/Logger.h>

#include <unordered_map>

DAS_CORE_IPC_NS_BEGIN

// thread_local 缓存用于 PumpUntilResponse
namespace
{
    thread_local std::unordered_map<CallKey, InboundMessage, CallKeyHash>
        t_pending_responses;
} // namespace

BusinessThread::BusinessThread(
    IpcMessageQueue<InboundMessage>& inbound,
    IpcRunLoop&                      run_loop)
    : inbound_(inbound), run_loop_(run_loop)
{
}

BusinessThread::~BusinessThread() { Stop(); }

void BusinessThread::Start(DistributedObjectManager& object_manager)
{
    if (running_.load())
    {
        return;
    }

    object_manager_ = &object_manager;
    running_.store(true);
    thread_ = std::thread(&BusinessThread::Run, this);

    DAS_CORE_LOG_INFO("BusinessThread started");
}

void BusinessThread::Stop()
{
    if (!running_.load())
    {
        return;
    }

    running_.store(false);

    // 关闭队列，让 Pop() 返回 nullopt
    inbound_.Uninitialize();

    if (thread_.joinable())
    {
        thread_.join();
    }

    DAS_CORE_LOG_INFO("BusinessThread stopped");
}

bool BusinessThread::IsRunning() const { return running_.load(); }

bool BusinessThread::IsCurrentThread() const
{
    return thread_.get_id() == std::this_thread::get_id();
}

void BusinessThread::Run()
{
    DAS_CORE_LOG_INFO("BusinessThread::Run() started");

    while (running_.load())
    {
        auto msg_opt = inbound_.Pop();

        if (!msg_opt.has_value())
        {
            // 队列已关闭
            DAS_CORE_LOG_DEBUG("BusinessThread: queue shutdown, exiting");
            break;
        }

        InboundMessage msg = std::move(msg_opt.value());
        DispatchMessage(msg);
    }

    DAS_CORE_LOG_INFO("BusinessThread::Run() exited");
}

void BusinessThread::DispatchMessage(InboundMessage& msg)
{
    const auto& header = msg.header;

    DAS_CORE_LOG_INFO(
        "BusinessThread::DispatchMessage: msg_type={}, interface_id={}, call_id={}",
        static_cast<int>(header.GetMessageType()),
        header.GetInterfaceId(),
        header.GetCallId());

    if (header.GetMessageType() == MessageType::RESPONSE)
    {
        // RESPONSE: 调用 CompletePendingCall
        CallKey call_key{
            .source_session_id = header.GetSourceSessionId(),
            .call_id = header.GetCallId()};
        DasResult result = header.HasError()
                               ? static_cast<DasResult>(header.GetErrorCode())
                               : DAS_S_OK;
        run_loop_.CompletePendingCall(
            call_key,
            result,
            std::move(msg.body),
            header.GetFlags());
    }
    else
    {
        // REQUEST/EVENT: 分发到 handler
        IMessageHandler* handler = run_loop_.GetHandler(
            header.GetHeaderFlags(),
            header.GetInterfaceId());

        if (handler && object_manager_)
        {
            // 创建 IpcResponseSender（业务线程模式）
            IpcResponseSender sender(run_loop_);

            // 构造 StubContext 并传递给 handler
            try
            {
                StubContext ctx{
                    *object_manager_,
                    run_loop_,
                    weak_from_this(),
                    header};
                auto result =
                    handler->HandleMessage(header, msg.body, sender, ctx);

                if (DAS::IsFailed(result))
                {
                    DAS_CORE_LOG_WARN(
                        "BusinessThread: handler returned error: {}",
                        result);
                }
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_ERROR(
                    "BusinessThread: handler threw exception: {}",
                    e.what());
            }
            catch (...)
            {
                DAS_CORE_LOG_ERROR(
                    "BusinessThread: handler threw unknown exception");
            }
        }
        else
        {
            DAS_CORE_LOG_WARN(
                "BusinessThread: no handler for interface_id={}",
                header.GetInterfaceId());
        }
    }
}

DasResult BusinessThread::PumpUntilResponse(
    CallKey               my_call_key,
    std::vector<uint8_t>& out_response,
    uint16_t*             out_flags)
{
    DAS_CORE_LOG_DEBUG(
        "PumpUntilResponse: waiting for call_key=({}, {})",
        my_call_key.source_session_id,
        my_call_key.call_id);

    // 首先检查 thread_local 缓存
    auto it = t_pending_responses.find(my_call_key);
    if (it != t_pending_responses.end())
    {
        out_response = std::move(it->second.body);
        if (out_flags)
        {
            *out_flags = it->second.header.GetFlags();
        }
        t_pending_responses.erase(it);
        DAS_CORE_LOG_DEBUG(
            "PumpUntilResponse: found in cache, call_id={}",
            my_call_key.call_id);
        return DAS_S_OK;
    }

    // 泵入循环
    while (running_.load())
    {
        auto msg_opt = inbound_.Pop();

        if (!msg_opt.has_value())
        {
            // 队列已关闭
            DAS_CORE_LOG_DEBUG(
                "PumpUntilResponse: queue shutdown, returning CANCELED");
            return DAS_E_IPC_CANCELED;
        }

        InboundMessage msg = std::move(msg_opt.value());

        if (msg.header.GetMessageType() == MessageType::RESPONSE)
        {
            CallKey call_key{
                .source_session_id = msg.header.GetSourceSessionId(),
                .call_id = msg.header.GetCallId()};

            if (call_key == my_call_key)
            {
                // 匹配我的响应
                out_response = std::move(msg.body);
                if (out_flags)
                {
                    *out_flags = msg.header.GetFlags();
                }
                // 检查 RESPONSE header 中的 error_code
                if (msg.header.HasError())
                {
                    DAS_CORE_LOG_DEBUG(
                        "PumpUntilResponse: error response for call_id = {}, "
                        "error_code = {}",
                        my_call_key.call_id,
                        msg.header.GetErrorCode());
                    return static_cast<DasResult>(msg.header.GetErrorCode());
                }

                DAS_CORE_LOG_DEBUG(
                    "PumpUntilResponse: matched, call_id = {}",
                    my_call_key.call_id);
                return DAS_S_OK;
            }
            else
            {
                // 不是我的响应，缓存起来
                t_pending_responses[call_key] = std::move(msg);
                DAS_CORE_LOG_DEBUG(
                    "PumpUntilResponse: cached response for call_id={}",
                    call_key.call_id);
            }
        }
        else
        {
            // REQUEST/EVENT: 分发处理（可能触发嵌套 pump）
            DispatchMessage(msg);
        }
    }

    // 线程已停止
    return DAS_E_IPC_CANCELED;
}

DAS_CORE_IPC_NS_END

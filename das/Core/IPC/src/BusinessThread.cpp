#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/Core/Logger/Logger.h>

DAS_CORE_IPC_NS_BEGIN

namespace
{
    thread_local BusinessThread* g_current_business_thread = nullptr;

    class ScopedBusinessThreadMarker
    {
    public:
        explicit ScopedBusinessThreadMarker(BusinessThread* thread) noexcept
            : previous_(g_current_business_thread)
        {
            g_current_business_thread = thread;
        }

        ~ScopedBusinessThreadMarker() { g_current_business_thread = previous_; }

        ScopedBusinessThreadMarker(const ScopedBusinessThreadMarker&) = delete;
        ScopedBusinessThreadMarker& operator=(
            const ScopedBusinessThreadMarker&) = delete;

    private:
        BusinessThread* previous_ = nullptr;
    };
} // namespace

bool IsCurrentBusinessThread() noexcept
{
    return g_current_business_thread != nullptr;
}

BusinessThread* GetCurrentBusinessThread() noexcept
{
    return g_current_business_thread;
}

BusinessThread::BusinessThread(
    IpcMessageQueue<InboundMessage>& inbound,
    IpcRunLoop&                      run_loop,
    IResolveContext&                 resolve_context,
    ProxyFactory&                    proxy_factory,
    RemoteObjectRegistry&            registry)
    : inbound_(inbound), run_loop_(run_loop), resolve_context_(resolve_context),
      proxy_factory_(proxy_factory), registry_(registry)
{
    running_.store(true);
    thread_ = std::thread(&BusinessThread::Run, this);
}

BusinessThread::~BusinessThread() { Stop(); }

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

    ScopedBusinessThreadMarker business_thread_marker(this);
    ScopedCurrentIpcContext    scope(&resolve_context_);

    proxy_factory_.GetObjectManager().SetBusinessThreadId(
        std::this_thread::get_id());

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
        ProcessInboundMessage(msg);
    }

    DAS_CORE_LOG_INFO("BusinessThread::Run() exited");
}

void BusinessThread::ProcessInboundMessage(InboundMessage& msg)
{
    const auto& header = msg.header;

    DAS_CORE_LOG_INFO(
        "BusinessThread::ProcessInboundMessage: msg_type={}, interface_id={}, call_id={}",
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

        if (handler)
        {
            // 创建 IpcResponseSender（业务线程模式）
            IpcResponseSender sender(run_loop_);

            // 构造 StubContext 并传递给 handler
            try
            {
                StubContext ctx{
                    proxy_factory_.GetObjectManager(),
                    registry_,
                    run_loop_,
                    weak_from_this(),
                    proxy_factory_,
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
            if (header.GetMessageType() == MessageType::REQUEST)
            {
                DAS_CORE_LOG_ERROR(
                    "BusinessThread: no handler for REQUEST "
                    "interface_id={}, call_id={}, source={}",
                    header.GetInterfaceId(),
                    header.GetCallId(),
                    header.GetSourceSessionId());

                // 构造 NACK RESPONSE 通知调用方
                auto nack_header =
                    IPCMessageHeaderBuilder()
                        .SetCallId(header.GetCallId())
                        .SetSourceSessionId(header.GetTargetSessionId())
                        .SetTargetSessionId(header.GetSourceSessionId())
                        .SetInterfaceId(header.GetInterfaceId())
                        .SetErrorCode(
                            static_cast<int32_t>(
                                DAS_E_IPC_COMMAND_NOT_REGISTERED))
                        .Build();

                IpcResponseSender    sender(run_loop_);
                std::vector<uint8_t> empty_body;
                auto send_result = sender.SendResponse(nack_header, empty_body);
                if (DAS::IsFailed(send_result))
                {
                    DAS_CORE_LOG_ERROR(
                        "BusinessThread: failed to send NACK, "
                        "result={}",
                        send_result);
                }
            }
            else
            {
                DAS_CORE_LOG_WARN(
                    "BusinessThread: no handler for EVENT "
                    "interface_id={}",
                    header.GetInterfaceId());
            }
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
                // 不是我的响应，走通用分发处理（可能是嵌套调用的响应）
                ProcessInboundMessage(msg);
            }
        }
        else
        {
            // REQUEST/EVENT: 分发处理（可能触发嵌套 pump）
            ProcessInboundMessage(msg);
        }
    }

    // 线程已停止
    return DAS_E_IPC_CANCELED;
}

DasResult BusinessThread::PumpUntilPredicate(
    std::string_view             wait_reason,
    const std::function<bool()>& is_done)
{
    (void)wait_reason;

    if (!is_done)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    while (running_.load())
    {
        if (is_done())
        {
            return DAS_S_OK;
        }

        auto msg_opt = inbound_.PopUntil(is_done);
        if (!msg_opt.has_value())
        {
            if (is_done())
            {
                return DAS_S_OK;
            }
            return DAS_E_IPC_CANCELED;
        }

        InboundMessage msg = std::move(msg_opt.value());
        ProcessInboundMessage(msg);
    }

    if (is_done())
    {
        return DAS_S_OK;
    }
    return DAS_E_IPC_CANCELED;
}

void BusinessThread::NotifyWaiters() { inbound_.NotifyWaiters(); }

DAS_CORE_IPC_NS_END

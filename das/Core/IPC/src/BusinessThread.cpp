#include <das/Core/IPC/AddRefDispatchGuard.h>
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

    // 活跃同步键登记守卫：构造登记键、析构注销键并清信箱内该键条目。
    // 等待者的全部出口（应答返回与取消返回）与异常穿透共用这一条
    // 注销路径，杜绝键残留与孤儿信箱消息滞留。
    template <typename ActiveKeys, typename StashedResponses>
    class ActiveSyncKeyGuard
    {
    public:
        ActiveSyncKeyGuard(
            ActiveKeys&       active_keys,
            StashedResponses& stashed_responses,
            const CallKey&    call_key)
            : active_keys_(active_keys), stashed_responses_(stashed_responses),
              call_key_(call_key)
        {
            active_keys_.insert(call_key_);
        }

        ~ActiveSyncKeyGuard()
        {
            active_keys_.erase(call_key_);
            stashed_responses_.erase(call_key_);
        }

        ActiveSyncKeyGuard(const ActiveSyncKeyGuard&) = delete;
        ActiveSyncKeyGuard& operator=(const ActiveSyncKeyGuard&) = delete;
        ActiveSyncKeyGuard(ActiveSyncKeyGuard&&) = delete;
        ActiveSyncKeyGuard& operator=(ActiveSyncKeyGuard&&) = delete;

    private:
        ActiveKeys&       active_keys_;
        StashedResponses& stashed_responses_;
        CallKey           call_key_;
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

bool IsOnBusinessThread() noexcept { return IsCurrentBusinessThread(); }

DasResult PumpBusinessThreadUntilPredicate(
    std::string_view             reason,
    const std::function<bool()>& is_done)
{
    if (g_current_business_thread == nullptr)
    {
        return DAS_E_IPC_INVALID_STATE;
    }
    return g_current_business_thread->PumpUntilPredicate(reason, is_done);
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

    const ScopedBusinessThreadMarker business_thread_marker(this);
    const ScopedCurrentIpcContext    scope(&resolve_context_);

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
    // 闸3 周期末出清（BT 漏斗）：本 dispatch 周期结束（含嵌套 pump 的
    // 整个外层函数帧）时 pending 队列非空则独立发出——入队后的帧不会
    // 滞留到下一周期
    const AddRefDispatchGuard add_ref_dispatch_guard(run_loop_);

    const auto& header = msg.header;

    DAS_CORE_LOG_INFO(
        "BusinessThread::ProcessInboundMessage: msg_type={}, interface_id={}, call_id={}",
        static_cast<int>(header.GetMessageType()),
        header.GetInterfaceId(),
        header.GetCallId());

    if (header.GetMessageType() == MessageType::Response)
    {
        const CallKey call_key{
            .source_session_id = header.GetSourceSessionId(),
            .call_id = header.GetCallId()};

        // 嵌套泵期间到达的外层同步调用响应：同步等待者不登记在
        // pending_calls_（异步回调表）里，走 CompletePendingCall 落空
        // 即被静默丢弃、外层永久挂起——改投暂存信箱，由属主等待者
        // 在自己的泵循环顶取回
        if (active_sync_keys_.count(call_key) != 0U)
        {
            stashed_responses_.emplace(call_key, std::move(msg));
            return;
        }

        // RESPONSE: 调用 CompletePendingCall
        const DasResult result =
            header.HasError() ? static_cast<DasResult>(header.GetErrorCode())
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
            if (header.GetMessageType() == MessageType::Request)
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

                IpcResponseSender          sender(run_loop_);
                const std::vector<uint8_t> empty_body;
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

    // 登记本等待键：期间任何泵（含嵌套）收到的本键 Response 都投信箱
    const ActiveSyncKeyGuard active_key_guard(
        active_sync_keys_,
        stashed_responses_,
        my_call_key);

    for (;;)
    {
        // 已到达并投箱的响应属于本等待者已持有的消息，关队列/停机
        // 不应吞掉它——取件检查先于停机判定
        auto stash_node = stashed_responses_.extract(my_call_key);
        if (!stash_node.empty())
        {
            InboundMessage stashed = std::move(stash_node.mapped());
            out_response = std::move(stashed.body);
            if (out_flags)
            {
                *out_flags = stashed.header.GetFlags();
            }
            // 检查 RESPONSE header 中的 error_code
            if (stashed.header.HasError())
            {
                DAS_CORE_LOG_DEBUG(
                    "PumpUntilResponse: error response for call_id = {}, "
                    "error_code = {}",
                    my_call_key.call_id,
                    stashed.header.GetErrorCode());
                return static_cast<DasResult>(stashed.header.GetErrorCode());
            }

            DAS_CORE_LOG_DEBUG(
                "PumpUntilResponse: matched, call_id = {}",
                my_call_key.call_id);
            return DAS_S_OK;
        }

        if (!running_.load())
        {
            DAS_CORE_LOG_DEBUG(
                "PumpUntilResponse: thread stopped, returning CANCELED");
            return DAS_E_IPC_CANCELED;
        }

        auto msg_opt = inbound_.Pop();
        if (!msg_opt.has_value())
        {
            // 队列已关闭
            DAS_CORE_LOG_DEBUG(
                "PumpUntilResponse: queue shutdown, returning CANCELED");
            return DAS_E_IPC_CANCELED;
        }

        // 弹出消息一律走通用分发：Response 命中活跃键进信箱，
        // REQUEST/EVENT 触发 handler（可能嵌套泵）
        ProcessInboundMessage(msg_opt.value());
    }
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

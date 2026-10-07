// BusinessThread 嵌套泵响应配对回归：外层同步调用（PumpUntilResponse
// 等待中）的 Response 在嵌套 handler 触发的内层泵期间到达时，必须归还
// 外层等待者（信箱语义），不得被内层泵经异步回调表落空后静默丢弃。
// 消息流全部经 inbound_queue_ 直驱（真实 BusinessThread + 真实
// IpcRunLoop，无 transport）。

#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/IDasBase.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace
{
    using DAS::Core::IPC::BusinessThread;
    using DAS::Core::IPC::CallKey;
    using DAS::Core::IPC::IMessageHandler;
    using DAS::Core::IPC::InboundMessage;
    using DAS::Core::IPC::IPCMessageHeaderBuilder;
    using DAS::Core::IPC::IpcMessageQueue;
    using DAS::Core::IPC::IpcRunLoop;
    using DAS::Core::IPC::MessageType;
    using DAS::Core::IPC::ProxyFactory;
    using DAS::Core::IPC::RemoteObjectRegistry;
    using DAS::Core::IPC::ValidatedIPCMessageHeader;

    constexpr uint16_t NESTED_PUMP_LOCAL_SESSION_ID = 1;
    constexpr uint16_t NESTED_PUMP_REMOTE_SESSION_ID = 2;

    // 业务接口 id 取测试专用值（避开 130/132 控制面 id）
    constexpr uint32_t OUTER_HANDLER_INTERFACE_ID = 0xE401;
    constexpr uint32_t INNER_HANDLER_INTERFACE_ID = 0xE402;
    constexpr uint32_t STOP_MARKER_INTERFACE_ID = 0xE403;

    constexpr uint16_t OUTER_CALL_ID = 901;
    constexpr uint16_t INNER_CALL_ID = 902;

    const std::vector<uint8_t> OUTER_RESPONSE_BODY{'o', 'u', 't', 'e', 'r'};
    const std::vector<uint8_t> INNER_RESPONSE_BODY{'i', 'n', 'n', 'e', 'r'};

    constexpr auto WATCHDOG_TIMEOUT = std::chrono::seconds(5);

    // 空实现 IResolveContext（BusinessThread 构造依赖，直驱任务不触达）
    class NestedPumpResolveContext final
        : public DAS::Core::IPC::IResolveContext
    {
    public:
        DasResult ResolveMainProcessInterface(
            const DasGuid& /*interface_id*/,
            IDasBase** /*out_object*/) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult RegisterService(
            IDasBase* /*service*/,
            const DasGuid& /*interface_id*/) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult UnregisterService(const DasGuid& /*interface_id*/) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult ResolveMainProcessInterfaceByName(
            const char* /*service_name*/,
            IDasBase** /*out_object*/) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult RegisterServiceByName(
            IDasBase* /*service*/,
            const DasGuid& /*interface_id*/,
            const char* /*service_name*/) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult UnregisterServiceByName(const char* /*service_name*/) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    struct PumpOutcome
    {
        DasResult            result = DAS_S_OK;
        std::vector<uint8_t> body;
    };

    // HandleMessage 内做嵌套 PumpUntilResponse 的 handler：结果经
    // promise 送回主线程断言（handler 在 BT 线程执行）
    class NestedPumpRequestHandler final : public IMessageHandler
    {
    public:
        NestedPumpRequestHandler(
            uint16_t                  wait_call_id,
            std::promise<PumpOutcome> outcome_promise)
            : wait_call_id_(wait_call_id),
              outcome_promise_(std::move(outcome_promise))
        {
        }

        [[nodiscard]]
        uint32_t AddRef() override
        {
            return ++ref_count_;
        }

        [[nodiscard]]
        uint32_t Release() override
        {
            const uint32_t count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        [[nodiscard]]
        uint32_t GetInterfaceId() const override
        {
            return 0;
        }

        DasResult HandleMessage(
            const ValidatedIPCMessageHeader& /*header*/,
            const std::vector<uint8_t>& /*body*/,
            DAS::Core::IPC::IpcResponseSender& /*sender*/,
            DAS::Core::IPC::StubContext& /*context*/) override
        {
            PumpOutcome          outcome;
            std::vector<uint8_t> response;
            outcome.result =
                DAS::Core::IPC::GetCurrentBusinessThread()->PumpUntilResponse(
                    CallKey{
                        .source_session_id = NESTED_PUMP_REMOTE_SESSION_ID,
                        .call_id = wait_call_id_},
                    response);
            outcome.body = std::move(response);
            outcome_promise_.set_value(std::move(outcome));
            return DAS_S_OK;
        }

    private:
        std::atomic<uint32_t>     ref_count_{0};
        uint16_t                  wait_call_id_;
        std::promise<PumpOutcome> outcome_promise_;
    };

    // 停机观测 handler：被弹出分发即置标志——排在响应之后的请求消息
    // 被消费可证明其前面的响应已离开队列（FIFO），主线程此后再 Stop
    // 不再有「响应尚未入箱」的竞态窗口
    class StopMarkerHandler final : public IMessageHandler
    {
    public:
        explicit StopMarkerHandler(std::atomic<bool>* observed)
            : observed_(observed)
        {
        }

        [[nodiscard]]
        uint32_t AddRef() override
        {
            return ++ref_count_;
        }

        [[nodiscard]]
        uint32_t Release() override
        {
            const uint32_t count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        [[nodiscard]]
        uint32_t GetInterfaceId() const override
        {
            return 0;
        }

        DasResult HandleMessage(
            const ValidatedIPCMessageHeader& /*header*/,
            const std::vector<uint8_t>& /*body*/,
            DAS::Core::IPC::IpcResponseSender& /*sender*/,
            DAS::Core::IPC::StubContext& /*context*/) override
        {
            observed_->store(true);
            return DAS_S_OK;
        }

    private:
        std::atomic<uint32_t> ref_count_{0};
        std::atomic<bool>*    observed_;
    };

    // 等待一个 future 就绪；超时先解卡队列再停线程（禁止挂死在 join）
    template <typename Future>
    bool WaitWithWatchdog(
        Future&                                future,
        IpcMessageQueue<InboundMessage>&       inbound_queue,
        const std::shared_ptr<BusinessThread>& business_thread)
    {
        if (future.wait_for(WATCHDOG_TIMEOUT) == std::future_status::ready)
        {
            return true;
        }
        inbound_queue.Uninitialize();
        business_thread->Stop();
        return false;
    }

    // 自旋等待原子标志置位；超时同样先解卡再停线程
    bool WaitForFlagWithWatchdog(
        const std::atomic<bool>&               flag,
        IpcMessageQueue<InboundMessage>&       inbound_queue,
        const std::shared_ptr<BusinessThread>& business_thread)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + WATCHDOG_TIMEOUT;
        while (!flag.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (flag.load())
        {
            return true;
        }
        inbound_queue.Uninitialize();
        business_thread->Stop();
        return false;
    }
} // namespace

class BusinessThreadNestedPumpTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        inbound_queue_ = std::make_unique<IpcMessageQueue<InboundMessage>>(16);

        // 本测试无出站发送：resolver 恒返连接丢失即可
        runloop_ = std::make_unique<IpcRunLoop>(
            false,
            inbound_queue_.get(),
            proxy_factory_,
            registry_,
            [](uint16_t /*target_session_id*/)
                -> DAS::Core::IPC::TransportLookupResult
            { return {DAS_E_IPC_CONNECTION_LOST, std::nullopt}; });
        runloop_->SetSessionId(NESTED_PUMP_LOCAL_SESSION_ID);
    }

    void TearDown() override
    {
        if (business_thread_)
        {
            business_thread_->Stop();
            business_thread_.reset();
        }
        if (runloop_)
        {
            runloop_->RequestStop();
            runloop_.reset();
        }
        inbound_queue_.reset();
    }

    void StartBusinessThread()
    {
        business_thread_ = std::make_shared<BusinessThread>(
            *inbound_queue_,
            *runloop_,
            resolve_context_,
            proxy_factory_,
            registry_);
    }

    InboundMessage MakeBusinessRequest(
        uint32_t                    interface_id,
        uint16_t                    call_id,
        const std::vector<uint8_t>& body)
    {
        InboundMessage request;
        request.header = IPCMessageHeaderBuilder()
                             .SetMessageType(MessageType::Request)
                             .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::NONE)
                             .SetInterfaceId(interface_id)
                             .SetCallId(call_id)
                             .SetSourceSessionId(NESTED_PUMP_REMOTE_SESSION_ID)
                             .SetTargetSessionId(NESTED_PUMP_LOCAL_SESSION_ID)
                             .SetBodySize(static_cast<uint32_t>(body.size()))
                             .Build();
        request.body = body;
        return request;
    }

    // Response 的 source = 远端（CallKey 取 Response 的 source）
    InboundMessage MakeBusinessResponse(
        uint32_t                    interface_id,
        uint16_t                    call_id,
        const std::vector<uint8_t>& body)
    {
        InboundMessage response;
        response.header = IPCMessageHeaderBuilder()
                              .SetMessageType(MessageType::Response)
                              .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::NONE)
                              .SetInterfaceId(interface_id)
                              .SetCallId(call_id)
                              .SetSourceSessionId(NESTED_PUMP_REMOTE_SESSION_ID)
                              .SetTargetSessionId(NESTED_PUMP_LOCAL_SESSION_ID)
                              .SetBodySize(static_cast<uint32_t>(body.size()))
                              .Build();
        response.body = body;
        return response;
    }

    NestedPumpResolveContext                         resolve_context_;
    ProxyFactory                                     proxy_factory_;
    RemoteObjectRegistry                             registry_;
    std::unique_ptr<IpcMessageQueue<InboundMessage>> inbound_queue_;
    std::unique_ptr<IpcRunLoop>                      runloop_;
    std::shared_ptr<BusinessThread>                  business_thread_;
};

// 嵌套窃取回归：外层响应先于内层响应到达，两个等待者各取回自己的
// 应答（不串扰、不超时）。修复前外层响应被内层泵经异步回调表落空
// 丢弃，外层 future 超时。
TEST_F(BusinessThreadNestedPumpTest, NestedPumpsReturnEachWaiterItsOwnResponse)
{
    std::promise<PumpOutcome> outer_promise;
    std::promise<PumpOutcome> inner_promise;
    auto                      outer_future = outer_promise.get_future();
    auto                      inner_future = inner_promise.get_future();

    auto* outer_handler =
        new NestedPumpRequestHandler(OUTER_CALL_ID, std::move(outer_promise));
    auto* inner_handler =
        new NestedPumpRequestHandler(INNER_CALL_ID, std::move(inner_promise));
    runloop_->RegisterHandler(
        DAS::Core::IPC::HeaderFlags::NONE,
        OUTER_HANDLER_INTERFACE_ID,
        outer_handler);
    runloop_->RegisterHandler(
        DAS::Core::IPC::HeaderFlags::NONE,
        INNER_HANDLER_INTERFACE_ID,
        inner_handler);

    StartBusinessThread();

    // 预推消息序：外层请求 → 内层请求 → 外层响应 → 内层响应。
    // BT Run 弹出 REQ_A 分发给 handler A（外层等待），外层泵弹出 REQ_B
    // 分发给 handler B（内层等待），内层泵先后弹出两个响应。
    ASSERT_EQ(
        inbound_queue_->Push(
            MakeBusinessRequest(OUTER_HANDLER_INTERFACE_ID, 701, {0xA1})),
        DAS_S_OK);
    ASSERT_EQ(
        inbound_queue_->Push(
            MakeBusinessRequest(INNER_HANDLER_INTERFACE_ID, 702, {0xB2})),
        DAS_S_OK);
    ASSERT_EQ(
        inbound_queue_->Push(MakeBusinessResponse(
            OUTER_HANDLER_INTERFACE_ID,
            OUTER_CALL_ID,
            OUTER_RESPONSE_BODY)),
        DAS_S_OK);
    ASSERT_EQ(
        inbound_queue_->Push(MakeBusinessResponse(
            INNER_HANDLER_INTERFACE_ID,
            INNER_CALL_ID,
            INNER_RESPONSE_BODY)),
        DAS_S_OK);

    ASSERT_TRUE(
        WaitWithWatchdog(outer_future, *inbound_queue_, business_thread_))
        << "outer waiter did not finish within watchdog timeout";
    ASSERT_TRUE(
        WaitWithWatchdog(inner_future, *inbound_queue_, business_thread_))
        << "inner waiter did not finish within watchdog timeout";

    const PumpOutcome outer_outcome = outer_future.get();
    const PumpOutcome inner_outcome = inner_future.get();
    EXPECT_EQ(outer_outcome.result, DAS_S_OK);
    EXPECT_EQ(outer_outcome.body, OUTER_RESPONSE_BODY);
    EXPECT_EQ(inner_outcome.result, DAS_S_OK);
    EXPECT_EQ(inner_outcome.body, INNER_RESPONSE_BODY);

    business_thread_->Stop();
}

// 队列关闭键注销 + 信箱按键隔离：内层等待期间 Stop（置运行标志 false +
// 反初始化队列），内层得 CANCELED；已到达的外层响应仍从信箱取回
// （不因关队列丢失），Stop 的 join 正常返回。修复前外层响应被内层泵
// 丢弃，外层只能拿 CANCELED。
TEST_F(
    BusinessThreadNestedPumpTest,
    QueueShutdownStillHandsStashedResponseToOuterWaiter)
{
    std::promise<PumpOutcome> outer_promise;
    std::promise<PumpOutcome> inner_promise;
    auto                      outer_future = outer_promise.get_future();
    auto                      inner_future = inner_promise.get_future();

    auto* outer_handler =
        new NestedPumpRequestHandler(OUTER_CALL_ID, std::move(outer_promise));
    auto* inner_handler =
        new NestedPumpRequestHandler(INNER_CALL_ID, std::move(inner_promise));
    runloop_->RegisterHandler(
        DAS::Core::IPC::HeaderFlags::NONE,
        OUTER_HANDLER_INTERFACE_ID,
        outer_handler);
    runloop_->RegisterHandler(
        DAS::Core::IPC::HeaderFlags::NONE,
        INNER_HANDLER_INTERFACE_ID,
        inner_handler);

    std::atomic<bool> stop_marker_observed{false};
    auto* marker_handler = new StopMarkerHandler(&stop_marker_observed);
    runloop_->RegisterHandler(
        DAS::Core::IPC::HeaderFlags::NONE,
        STOP_MARKER_INTERFACE_ID,
        marker_handler);

    StartBusinessThread();

    // 预推消息序：外层请求 → 内层请求 → 外层响应（无内层响应）→
    // 停机观测请求。观测请求被消费即证明外层响应已被内层泵处理。
    ASSERT_EQ(
        inbound_queue_->Push(
            MakeBusinessRequest(OUTER_HANDLER_INTERFACE_ID, 701, {0xA1})),
        DAS_S_OK);
    ASSERT_EQ(
        inbound_queue_->Push(
            MakeBusinessRequest(INNER_HANDLER_INTERFACE_ID, 702, {0xB2})),
        DAS_S_OK);
    ASSERT_EQ(
        inbound_queue_->Push(MakeBusinessResponse(
            OUTER_HANDLER_INTERFACE_ID,
            OUTER_CALL_ID,
            OUTER_RESPONSE_BODY)),
        DAS_S_OK);
    ASSERT_EQ(
        inbound_queue_->Push(
            MakeBusinessRequest(STOP_MARKER_INTERFACE_ID, 703, {0xC3})),
        DAS_S_OK);

    ASSERT_TRUE(WaitForFlagWithWatchdog(
        stop_marker_observed,
        *inbound_queue_,
        business_thread_))
        << "stop marker was not consumed within watchdog timeout";

    // 内层 Pop 返回 nullopt → CANCELED；外层从信箱取回已到达的响应
    business_thread_->Stop();

    const PumpOutcome outer_outcome = outer_future.get();
    const PumpOutcome inner_outcome = inner_future.get();
    EXPECT_EQ(inner_outcome.result, DAS_E_IPC_CANCELED);
    EXPECT_EQ(outer_outcome.result, DAS_S_OK);
    EXPECT_EQ(outer_outcome.body, OUTER_RESPONSE_BODY);
}

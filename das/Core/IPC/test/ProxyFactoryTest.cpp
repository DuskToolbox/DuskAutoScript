/**
 * @file ProxyFactoryTest.cpp
 * @brief ProxyFactory 单元测试
 *
 * ProxyFactory 已从单例改为构造函数注入模式。
 * 由于 ProxyFactory 依赖 IpcRunLoop（构造复杂），
 * 这里仅测试不依赖 IpcRunLoop 的基础行为。
 * 完整行为通过集成测试覆盖。
 */

#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/DasReadOnlyStringProxy.h>
#include <das/Core/IPC/DasVariantVectorByValueProxy.h>
#include <das/Core/IPC/IHostConnection.h>
#include <das/Core/IPC/InternalCallbackHandler.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/IpcRuntimeState.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>
#include <das/Utils/fmt.h>
#include <gtest/gtest.h>
#include <memory>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>

// winsock 符号依赖 AnyTransport 的 boost 头先行引入，独立块防止排序前置
#include <das/Core/IPC/AfUnixAvailable.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <optional>
#include <thread>
#include <utility>

#include "OutboundFrameProbe.h"
#include "TestTransportResolvers.h"

using DAS::DasPtr;
using DAS::Core::IPC::AnyTransport;
using DAS::Core::IPC::AsyncIpcMessage;
using DAS::Core::IPC::BusinessThread;
using DAS::Core::IPC::DasReadOnlyStringProxy;
using DAS::Core::IPC::DasVariantVectorByValueProxy;
using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::IpcCommandType;
using DAS::Core::IPC::IPCMessageHeaderBuilder;
using DAS::Core::IPC::IPCProxyBase;
using DAS::Core::IPC::IpcRunLoop;
using DAS::Core::IPC::IpcRuntimeState;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::ProxyFactory;
using DAS::Core::IPC::RemoteObjectRegistry;
using DAS::Core::IPC::TransportLookupResult;
using DAS::Core::IPC::ValidatedIPCMessageHeader;

// Test ObjectId 编码解码后的状态一致性
TEST(ProxyFactoryTest, ObjectIdEncodingConsistency)
{
    ObjectId original{.session_id = 123, .generation = 456, .local_id = 789012};

    // 编码
    const uint64_t encoded = EncodeObjectId(original);

    // 解码
    ObjectId decoded = DecodeObjectId(encoded);

    // 验证解码后的值与原始值一致
    EXPECT_EQ(decoded.session_id, original.session_id);
    EXPECT_EQ(decoded.generation, original.generation);
    EXPECT_EQ(decoded.local_id, original.local_id);
}

// Test 边界值的 ObjectId
TEST(ProxyFactoryTest, ObjectIdWithBoundaryValues)
{
    ObjectId max_values{
        .session_id = 0xFFFF,
        .generation = 0xFFFF,
        .local_id = 0xFFFFFFFF};

    // 编码解码
    const uint64_t encoded = EncodeObjectId(max_values);
    ObjectId       decoded = DecodeObjectId(encoded);

    EXPECT_EQ(decoded.session_id, max_values.session_id);
    EXPECT_EQ(decoded.generation, max_values.generation);
    EXPECT_EQ(decoded.local_id, max_values.local_id);
}

// Test 零值的 ObjectId
TEST(ProxyFactoryTest, ObjectIdWithZeroValues)
{
    ObjectId zero{.session_id = 0, .generation = 0, .local_id = 0};

    // 零值对象的编码应该为0
    const uint64_t encoded = EncodeObjectId(zero);
    EXPECT_EQ(encoded, 0);

    // 解码后仍为零值
    ObjectId decoded = DecodeObjectId(encoded);
    EXPECT_EQ(decoded.session_id, 0);
    EXPECT_EQ(decoded.generation, 0);
    EXPECT_EQ(decoded.local_id, 0);
}

TEST(ProxyFactoryTest, BeginShutdownFlipsRuntimeStateAndBlocksProxyCreation)
{
    ProxyFactory         proxy_factory;
    RemoteObjectRegistry registry;
    // main 语义构造点：显式传两级查表 resolver
    IpcRunLoop run_loop(
        false,
        nullptr,
        proxy_factory,
        registry,
        [&run_loop](uint16_t target_session_id) -> TransportLookupResult
        {
            return ResolveTransportMainSemantic(
                run_loop.GetConnectionManager(),
                target_session_id);
        });
    const ObjectId object_id{.session_id = 7, .generation = 1, .local_id = 42};

    auto runtime_state = proxy_factory.GetRuntimeState().lock();
    ASSERT_TRUE(runtime_state);
    EXPECT_FALSE(runtime_state->IsShuttingDown());

    proxy_factory.BeginShutdown();

    EXPECT_TRUE(runtime_state->IsShuttingDown());
    EXPECT_FALSE(proxy_factory
                     .GetOrCreateProxy(
                         run_loop,
                         {},
                         object_id,
                         DasReadOnlyStringProxy::InterfaceId)
                     .second);
    EXPECT_FALSE(proxy_factory.HasProxy(object_id));
}

class ProxyRefcountTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // main 语义构造点：显式传两级查表 resolver（调用期经 this 现读
        // 成员 run loop 的 ConnectionManager）
        run_loop_ = std::make_unique<IpcRunLoop>(
            false,
            nullptr,
            proxy_factory_,
            registry_,
            [this](uint16_t target_session_id) -> TransportLookupResult
            {
                return ResolveTransportMainSemantic(
                    run_loop_->GetConnectionManager(),
                    target_session_id);
            });
    }

    ProxyFactory                  proxy_factory_;
    RemoteObjectRegistry          registry_;
    std::unique_ptr<IpcRunLoop>   run_loop_;
    std::weak_ptr<BusinessThread> business_thread_;
    const ObjectId object_id_{.session_id = 7, .generation = 1, .local_id = 42};
};

TEST_F(ProxyRefcountTest, ReadOnlyStringProxyUsesIPCProxyBaseRefcount)
{
    auto* proxy = new DasReadOnlyStringProxy(
        DasReadOnlyStringProxy::InterfaceId,
        object_id_,
        *run_loop_,
        business_thread_,
        proxy_factory_);

    EXPECT_EQ(proxy->AddRef(), 2u);

    IPCProxyBase* runtime_tag = nullptr;
    ASSERT_EQ(
        proxy->QueryInterface(
            DasIidOf<IPCProxyBase>(),
            reinterpret_cast<void**>(&runtime_tag)),
        DAS_S_OK);
    ASSERT_NE(runtime_tag, nullptr);
    EXPECT_EQ(runtime_tag, static_cast<IPCProxyBase*>(proxy));

    EXPECT_EQ(runtime_tag->ReleaseRuntimeTagRef(), 2u);
    EXPECT_TRUE(runtime_tag->TryAddRefForCache());

    EXPECT_EQ(proxy->Release(), 2u);
    EXPECT_EQ(proxy->Release(), 1u);
    EXPECT_EQ(proxy->Release(), 0u);
}

TEST_F(ProxyRefcountTest, VariantVectorProxyUsesIPCProxyBaseRefcount)
{
    auto* proxy = new DasVariantVectorByValueProxy(
        DasVariantVectorByValueProxy::InterfaceId,
        object_id_,
        *run_loop_,
        business_thread_,
        proxy_factory_);

    EXPECT_EQ(proxy->AddRef(), 2u);

    IPCProxyBase* runtime_tag = nullptr;
    ASSERT_EQ(
        proxy->QueryInterface(
            DasIidOf<IPCProxyBase>(),
            reinterpret_cast<void**>(&runtime_tag)),
        DAS_S_OK);
    ASSERT_NE(runtime_tag, nullptr);
    EXPECT_EQ(runtime_tag, static_cast<IPCProxyBase*>(proxy));

    EXPECT_EQ(runtime_tag->ReleaseRuntimeTagRef(), 2u);
    EXPECT_TRUE(runtime_tag->TryAddRefForCache());

    EXPECT_EQ(proxy->Release(), 2u);
    EXPECT_EQ(proxy->Release(), 1u);
    EXPECT_EQ(proxy->Release(), 0u);
}

TEST_F(ProxyRefcountTest, GetOrCreateProxyCacheHitUsesLiveRuntimeRef)
{
    auto [r1, first] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_EQ(r1, DAS_S_OK);
    ASSERT_TRUE(first);
    EXPECT_TRUE(proxy_factory_.HasProxy(object_id_));

    auto [r2, second] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_EQ(r2, DAS_S_OK);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.Get(), second.Get());
    EXPECT_EQ(proxy_factory_.GetProxyCount(), 1u);

    second.Reset();
    EXPECT_TRUE(proxy_factory_.HasProxy(object_id_));

    first.Reset();
    EXPECT_FALSE(proxy_factory_.HasProxy(object_id_));
    EXPECT_EQ(proxy_factory_.GetProxyCount(), 0u);
}

TEST_F(ProxyRefcountTest, GetOrCreateProxyCachesInterfaceViewsPerObjectId)
{
    auto [r1, string_proxy] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_EQ(r1, DAS_S_OK);
    ASSERT_TRUE(string_proxy);

    auto [r2, vector_proxy] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasVariantVectorByValueProxy::InterfaceId);
    ASSERT_EQ(r2, DAS_S_OK);
    ASSERT_TRUE(vector_proxy);

    EXPECT_NE(string_proxy.Get(), vector_proxy.Get());
    EXPECT_TRUE(proxy_factory_.HasProxy(object_id_));
    EXPECT_EQ(proxy_factory_.GetProxyCount(), 2u);
}

TEST_F(ProxyRefcountTest, FinalReleaseInvalidationRequiresRuntimePointerMatch)
{
    auto [r3, proxy] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_EQ(r3, DAS_S_OK);
    ASSERT_TRUE(proxy);
    EXPECT_TRUE(proxy_factory_.HasProxy(object_id_));

    proxy_factory_.OnProxyFinalRelease(
        object_id_,
        DasReadOnlyStringProxy::InterfaceId,
        nullptr);
    EXPECT_TRUE(proxy_factory_.HasProxy(object_id_));

    proxy.Reset();
    EXPECT_FALSE(proxy_factory_.HasProxy(object_id_));
}

TEST_F(ProxyRefcountTest, ProxyMethodAfterBeginShutdownReturnsDisconnected)
{
    auto [r4, base] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_EQ(r4, DAS_S_OK);
    ASSERT_TRUE(base);

    IDasReadOnlyString* string_proxy = nullptr;
    ASSERT_EQ(
        base->QueryInterface(
            DAS_IID_READ_ONLY_STRING,
            reinterpret_cast<void**>(&string_proxy)),
        DAS_S_OK);
    ASSERT_NE(string_proxy, nullptr);

    proxy_factory_.BeginShutdown();

    const char* utf8 = nullptr;
    EXPECT_EQ(string_proxy->GetUtf8(&utf8), DAS_E_IPC_DISCONNECTED);
    EXPECT_EQ(string_proxy->Release(), 1u);
    base.Reset();
}

// ====== 远端引用配对（物化申报 RemoteAddRef / 条目移除 RemoteRelease）======

namespace
{
    constexpr uint16_t REMOTE_REF_LOCAL_SESSION_ID = 1;
    constexpr uint16_t REMOTE_REF_PEER_SESSION_ID = 7;

    // 带截止时间的接收协程：deadline 到时由 timer 分支收敛，协程必然
    // 完成——io_context 的 run() 不会因悬挂的管道读而无法返回
    boost::asio::awaitable<std::optional<AsyncIpcMessage>> ReceiveUntilDeadline(
        AnyTransport&             capture_transport,
        std::chrono::milliseconds deadline)
    {
        using namespace boost::asio::experimental::awaitable_operators;

        boost::asio::steady_timer deadline_timer(
            co_await boost::asio::this_coro::executor);
        deadline_timer.expires_after(deadline);

        auto receive_branch = [](AnyTransport& transport)
            -> boost::asio::awaitable<std::optional<AsyncIpcMessage>>
        {
            auto received = co_await transport.ReceiveCoroutine();
            if (auto* error_code = std::get_if<DasResult>(&received))
            {
                (void)error_code;
                co_return std::nullopt;
            }
            co_return std::get<AsyncIpcMessage>(std::move(received));
        };

        auto deadline_branch = [&deadline_timer]()
            -> boost::asio::awaitable<std::optional<AsyncIpcMessage>>
        {
            co_await deadline_timer.async_wait(boost::asio::use_awaitable);
            co_return std::nullopt;
        };

        auto winner =
            co_await (receive_branch(capture_transport) || deadline_branch());
        if (winner.index() == 0)
        {
            co_return std::get<0>(std::move(winner));
        }
        co_return std::nullopt;
    }
} // namespace

// 远端引用配对测试装置：run loop 侧注册 host-local transport（PostSend 的
// 发送查找路径），配对管道的另一端归测试所有——线上读到的帧就是
// ProxyFactory/DasProxyBase 发出的全部远端引用报文
class ProxyRemoteRefPairingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // main 语义构造点：显式传两级查表 resolver（调用期经 this 现读
        // 成员 run loop 的 ConnectionManager）
        run_loop_ = std::make_unique<IpcRunLoop>(
            false,
            nullptr,
            proxy_factory_,
            registry_,
            [this](uint16_t target_session_id) -> TransportLookupResult
            {
                return ResolveTransportMainSemantic(
                    run_loop_->GetConnectionManager(),
                    target_session_id);
            });
        run_loop_->SetSessionId(REMOTE_REF_LOCAL_SESSION_ID);

        ASSERT_TRUE(CreateConnectedCapturePair());
        ASSERT_EQ(
            run_loop_->GetConnectionManager().RegisterHostLocalTransport(
                REMOTE_REF_PEER_SESSION_ID,
                std::move(*capture_pair_run_loop_side_)),
            DAS_S_OK);
        capture_pair_run_loop_side_.reset();
    }

    void TearDown() override
    {
        // capture 端必须先于 run_loop_（其 io_context 的 owner）析构
        capture_transport_.reset();
        capture_pair_run_loop_side_.reset();
        run_loop_.reset();
    }

    // 创建一对连通传输（照 IpcRunLoopTest::CreateConnectedTransportPair），
    // run loop 侧留给 ConnectionManager，另一侧作捕获端
    bool CreateConnectedCapturePair()
    {
        auto&                        io_context = run_loop_->GetIoContext();
        static std::atomic<uint32_t> next_endpoint_id{1};
        const auto                   base = DAS_FMT_NS::format(
            "pfrp_{}_{}",
            GetCurrentProcessId() % 10000,
            next_endpoint_id.fetch_add(1));
        const auto read_endpoint = base + "_read";
        const auto write_endpoint = base + "_write";
        const bool use_single_endpoint = DAS::Core::IPC::AfUnixAvailable();
        const auto peer_read_endpoint =
            use_single_endpoint ? read_endpoint : write_endpoint;
        const auto peer_write_endpoint =
            use_single_endpoint ? write_endpoint : read_endpoint;

        auto server_future = boost::asio::co_spawn(
            io_context,
            AnyTransport::CreateAsync(
                io_context,
                read_endpoint,
                write_endpoint,
                true,
                65536),
            boost::asio::use_future);
        auto peer_future = boost::asio::co_spawn(
            io_context,
            AnyTransport::CreateAsync(
                io_context,
                peer_read_endpoint,
                peer_write_endpoint,
                false,
                65536),
            boost::asio::use_future);

        std::thread io_thread([&io_context]() { io_context.run(); });

        auto [server_result, server_transport] = server_future.get();
        auto [peer_result, peer_transport] = peer_future.get();

        io_thread.join();
        io_context.restart();

        if (server_result != DAS_S_OK || !server_transport)
        {
            ADD_FAILURE() << "Failed to create run-loop-side transport: "
                          << server_result;
            return false;
        }

        if (peer_result != DAS_S_OK || !peer_transport)
        {
            ADD_FAILURE() << "Failed to create capture transport: "
                          << peer_result;
            return false;
        }

        capture_pair_run_loop_side_ = std::move(*server_transport);
        capture_transport_ = std::move(*peer_transport);
        return true;
    }

    // 驱动一次带截止时间的线上接收：协程由 deadline 保证收敛，future 必然
    // 就绪，io 线程必然可 join
    std::optional<AsyncIpcMessage> ReceiveNextSentMessage(
        std::chrono::milliseconds deadline)
    {
        auto& io_context = run_loop_->GetIoContext();
        auto  receive_future = boost::asio::co_spawn(
            io_context,
            ReceiveUntilDeadline(*capture_transport_, deadline),
            boost::asio::use_future);
        if (io_context.stopped())
        {
            io_context.restart();
        }
        std::thread io_thread([&io_context]() { io_context.run(); });
        auto        message = receive_future.get();
        io_thread.join();
        return message;
    }

    ProxyFactory                  proxy_factory_;
    RemoteObjectRegistry          registry_;
    std::unique_ptr<IpcRunLoop>   run_loop_;
    std::weak_ptr<BusinessThread> business_thread_;
    const ObjectId                object_id_{
        .session_id = REMOTE_REF_PEER_SESSION_ID,
        .generation = 1,
        .local_id = 42};

    // 声明顺序保证捕获端先于 run_loop_ 析构（io_context 生命周期）
    std::optional<AnyTransport> capture_pair_run_loop_side_;
    std::optional<AnyTransport> capture_transport_;
};

// 首次物化（GetOrCreateProxy 缓存未命中、DOM remote 条目 0→1）必须且只须
// 向 owner 发一条 RemoteAddRef 申报：CONTROL_PLANE EVENT、source=本地
// session、target=oid.session_id、body 为 ObjectId 8 字节
TEST_F(
    ProxyRemoteRefPairingTest,
    FirstMaterializationSendsExactlyOneRemoteAddRef)
{
    auto [result, proxy] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(proxy);

    // 候车语义：物化申报入队后不再直发（下一写者/guard 周期/心跳班车
    // 三级送出）。本夹具零载体，显式出清是唯一确定性送出机制。
    static_cast<void>(run_loop_->FlushOutboundFifo());

    auto message = ReceiveNextSentMessage(std::chrono::seconds(1));
    ASSERT_TRUE(message.has_value())
        << "first materialization must declare itself with one RemoteAddRef";

    const auto& header = message->first;
    EXPECT_EQ(header.GetMessageType(), MessageType::Event);
    EXPECT_EQ(
        header.GetHeaderFlags(),
        DAS::Core::IPC::HeaderFlags::CONTROL_PLANE);
    EXPECT_EQ(
        header.GetInterfaceId(),
        static_cast<uint32_t>(IpcCommandType::RemoteAddRef));
    EXPECT_EQ(header.GetSourceSessionId(), run_loop_->GetSessionId());
    EXPECT_EQ(header.GetTargetSessionId(), object_id_.session_id);
    ASSERT_EQ(message->second.size(), sizeof(ObjectId));
    EXPECT_EQ(
        std::memcmp(message->second.data(), &object_id_, sizeof(ObjectId)),
        0);

    // 恰好一条：出清后短期内不得出现第二条申报
    static_cast<void>(run_loop_->FlushOutboundFifo());
    EXPECT_FALSE(
        ReceiveNextSentMessage(std::chrono::milliseconds(300)).has_value());

    proxy.Reset();
}

// 同对象第二个接口视图的物化（QI 衍生物化内部同样走 GetOrCreateProxy）
// 不得追加第二条 RemoteAddRef——申报严格挂 oid 级条目新建跃迁，不挂
// 每接口视图物化
TEST_F(
    ProxyRemoteRefPairingTest,
    SecondInterfaceViewSendsNoAdditionalRemoteAddRef)
{
    auto [first_result, string_proxy] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_EQ(first_result, DAS_S_OK);
    ASSERT_TRUE(string_proxy);

    auto [second_result, vector_proxy] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasVariantVectorByValueProxy::InterfaceId);
    ASSERT_EQ(second_result, DAS_S_OK);
    ASSERT_TRUE(vector_proxy);

    // 候车语义零载体：显式出清送出首条申报
    static_cast<void>(run_loop_->FlushOutboundFifo());

    auto message = ReceiveNextSentMessage(std::chrono::seconds(1));
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(
        message->first.GetInterfaceId(),
        static_cast<uint32_t>(IpcCommandType::RemoteAddRef));

    // 第二视图物化后不得再有追加申报（再次出清，堵住漏发藏在候车队列
    // 里的假静默）
    static_cast<void>(run_loop_->FlushOutboundFifo());
    EXPECT_FALSE(
        ReceiveNextSentMessage(std::chrono::milliseconds(300)).has_value());
}

// 多接口视图全部析构 → 恰好一条 RemoteRelease：RELEASE 聚合挂条目移除
// 跃迁（remote 条目归零），绝不按 proxy 实例数发
TEST_F(
    ProxyRemoteRefPairingTest,
    FinalViewDestructionSendsExactlyOneRemoteRelease)
{
    auto [first_result, string_proxy] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_EQ(first_result, DAS_S_OK);
    ASSERT_TRUE(string_proxy);

    auto [second_result, vector_proxy] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasVariantVectorByValueProxy::InterfaceId);
    ASSERT_EQ(second_result, DAS_S_OK);
    ASSERT_TRUE(vector_proxy);

    // 软排空物化期报文（候车语义零载体：显式出清后才真正送达），之后
    // 的线上流量只剩 RELEASE
    static_cast<void>(run_loop_->FlushOutboundFifo());
    static_cast<void>(ReceiveNextSentMessage(std::chrono::seconds(1)));

    vector_proxy.Reset();
    string_proxy.Reset();

    // 观察窗口的 drain 触发点：显式 guard 出清动作（FlushOutboundFifo）
    // ——RELEASE 到达不依赖心跳或后续业务载体
    static_cast<void>(run_loop_->FlushOutboundFifo());

    // 窗口内收帧至静默：恰一条 RELEASE（条目只移除一次，绝不按 proxy
    // 实例数多发）
    std::vector<AsyncIpcMessage> window_messages;
    if (auto first_message = ReceiveNextSentMessage(std::chrono::seconds(1)))
    {
        window_messages.push_back(std::move(*first_message));
        while (auto next_message =
                   ReceiveNextSentMessage(std::chrono::milliseconds(300)))
        {
            window_messages.push_back(std::move(*next_message));
        }
    }
    ASSERT_EQ(window_messages.size(), 1u)
        << "entry removal must send exactly one RemoteRelease";

    const auto& release_message = window_messages.front();
    const auto& release_header = release_message.first;
    EXPECT_EQ(release_header.GetMessageType(), MessageType::Event);
    EXPECT_EQ(
        release_header.GetHeaderFlags(),
        DAS::Core::IPC::HeaderFlags::CONTROL_PLANE);
    EXPECT_EQ(
        release_header.GetInterfaceId(),
        static_cast<uint32_t>(IpcCommandType::RemoteRelease));
    EXPECT_EQ(release_header.GetSourceSessionId(), run_loop_->GetSessionId());
    EXPECT_EQ(release_header.GetTargetSessionId(), object_id_.session_id);
    ASSERT_EQ(release_message.second.size(), sizeof(ObjectId));
    EXPECT_EQ(
        std::memcmp(
            release_message.second.data(),
            &object_id_,
            sizeof(ObjectId)),
        0);
}

// 停机期销毁（IsRuntimeAvailable 门）：不发送任何远端引用报文，也不崩溃
TEST_F(ProxyRemoteRefPairingTest, ShutdownBlocksRemoteRefSendsWithoutCrash)
{
    auto [result, proxy] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(proxy);

    // 排空物化期报文（无论当前实现发什么），此后静默期作为基线
    static_cast<void>(ReceiveNextSentMessage(std::chrono::seconds(1)));

    proxy_factory_.BeginShutdown();

    // 停机后销毁：门内零发送、零崩溃
    proxy.Reset();

    EXPECT_FALSE(
        ReceiveNextSentMessage(std::chrono::milliseconds(300)).has_value());
}

// ====== D-19 物化点线程判别分叉（段 B 候车机制）======

namespace
{
    constexpr uint16_t FORK_LOCAL_SESSION_ID = 1;
    constexpr uint16_t FORK_PEER_SESSION_ID = 7;

    // 空实现 IResolveContext（BusinessThread 构造依赖，分叉测试不触达）
    class ForkResolveContext final : public DAS::Core::IPC::IResolveContext
    {
    public:
        DasResult ResolveMainProcessInterface(const DasGuid&, IDasBase**)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult RegisterService(IDasBase*, const DasGuid&) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult UnregisterService(const DasGuid&) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult ResolveMainProcessInterfaceByName(const char*, IDasBase**)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult RegisterServiceByName(IDasBase*, const DasGuid&, const char*)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult UnregisterServiceByName(const char*) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    // 把任意任务投递到真实 BusinessThread 执行线程的回调（照 IpcCoreTest
    // 的 BusinessThreadProbeCallback 手法）
    class BusinessThreadTaskCallback final : public IDasAsyncCallback
    {
    public:
        explicit BusinessThreadTaskCallback(std::function<void()> task)
            : task_(std::move(task))
        {
        }

        uint32_t AddRef() override
        {
            return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        uint32_t Release() override
        {
            const uint32_t count =
                ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            if (pp_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DAS_IID_BASE)
            {
                *pp_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult Do() noexcept override
        {
            task_();
            return DAS_S_OK;
        }

    private:
        std::atomic<uint32_t> ref_count_{1};
        std::function<void()> task_;
    };
} // namespace

// D-19 分叉测试装置：真实 BusinessThread（线程身份真实）+ OutboundFrameProbe
// 观测 seam（对端注册为 peer session 的 host-local transport，探针读到的即
// run loop 全部出站帧）
class ProxyAddRefForkTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // main 语义构造点：显式传两级查表 resolver
        run_loop_ = std::make_unique<IpcRunLoop>(
            false,
            inbound_queue_.get(),
            proxy_factory_,
            registry_,
            [this](uint16_t target_session_id) -> TransportLookupResult
            {
                return ResolveTransportMainSemantic(
                    run_loop_->GetConnectionManager(),
                    target_session_id);
            });
        run_loop_->SetSessionId(FORK_LOCAL_SESSION_ID);

        internal_callback_handler_ =
            DasPtr<DAS::Core::IPC::InternalCallbackHandler>(
                new DAS::Core::IPC::InternalCallbackHandler);
        run_loop_->RegisterHandler(
            DAS::Core::IPC::HeaderFlags::BUSINESS_CONTROL,
            internal_callback_handler_->GetInterfaceId(),
            internal_callback_handler_.Get());

        business_thread_ = std::make_shared<BusinessThread>(
            *inbound_queue_,
            *run_loop_,
            resolve_context_,
            proxy_factory_,
            registry_);

        probe_ =
            std::make_unique<OutboundFrameProbe>(run_loop_->GetIoContext());
        ASSERT_TRUE(probe_->IsHealthy());
        auto counterpart_transport = probe_->TakeCounterpartTransport();
        ASSERT_TRUE(counterpart_transport.has_value());
        ASSERT_EQ(
            run_loop_->GetConnectionManager().RegisterHostLocalTransport(
                FORK_PEER_SESSION_ID,
                std::move(*counterpart_transport)),
            DAS_S_OK);
    }

    void TearDown() override
    {
        // 生命期序：探针 transport 先关（io_context 的 owner 是 run loop），
        // BT 先停（引用 run loop 与队列），run loop 随后析构
        if (probe_)
        {
            probe_->Shutdown();
        }
        if (business_thread_)
        {
            business_thread_->Stop();
        }
        run_loop_.reset();
        inbound_queue_.reset();
    }

    // 在真实 BusinessThread 执行线程上运行 task 并等待完成（嵌套 pump 之外
    // 的测试直驱形态）
    void RunOnBusinessThread(const std::function<void()>& task)
    {
        std::promise<void> task_done;
        auto               task_done_future = task_done.get_future();
        const DasPtr<IDasAsyncCallback> callback(new BusinessThreadTaskCallback(
            [&task, &task_done]()
            {
                task();
                task_done.set_value();
            }));
        ASSERT_EQ(run_loop_->PostToBusinessThread(callback.Get()), DAS_S_OK);
        ASSERT_EQ(
            task_done_future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
    }

    ProxyFactory         proxy_factory_;
    RemoteObjectRegistry registry_;
    std::unique_ptr<
        DAS::Core::IPC::IpcMessageQueue<DAS::Core::IPC::InboundMessage>>
                                inbound_queue_ = std::make_unique<
                                    DAS::Core::IPC::IpcMessageQueue<DAS::Core::IPC::InboundMessage>>(
                                    16);
    ForkResolveContext          resolve_context_;
    std::unique_ptr<IpcRunLoop> run_loop_;
    DasPtr<DAS::Core::IPC::InternalCallbackHandler> internal_callback_handler_;
    std::shared_ptr<BusinessThread>                 business_thread_;
    std::unique_ptr<OutboundFrameProbe>             probe_;
    const ObjectId                                  object_id_{
        .session_id = FORK_PEER_SESSION_ID,
        .generation = 1,
        .local_id = 42};
};

// BT 执行线程上首次物化：入队候车，零直发（pending 计数 1、探针零 130 帧）
TEST_F(
    ProxyAddRefForkTest,
    BusinessThreadMaterializationEnqueuesWithoutDirectSend)
{
    DasPtr<IDasBase>    materialized_proxy;
    std::atomic<size_t> pending_count_at_materialization{0};
    RunOnBusinessThread(
        [&]()
        {
            auto [result, proxy] = proxy_factory_.GetOrCreateProxy(
                *run_loop_,
                business_thread_,
                object_id_,
                DasReadOnlyStringProxy::InterfaceId);
            ASSERT_EQ(result, DAS_S_OK);
            ASSERT_TRUE(proxy);
            // 物化调用返回后、周期结束前（BT 任务体内可观测）：已入队
            // 一条、尚未发出——入队零直发
            pending_count_at_materialization.store(
                run_loop_->GetPendingAddRefCount(FORK_PEER_SESSION_ID),
                std::memory_order_release);
            materialized_proxy = std::move(proxy);
        });

    // 物化点零直发的周期内观测：恰入队一条（promise/future 与
    // atomic 提供跨线程可见性）
    EXPECT_EQ(
        pending_count_at_materialization.load(std::memory_order_acquire),
        1u);

    // 周期末出清（guard 析构）后：帧在当次窗口到达、队列空
    EXPECT_EQ(probe_->DrainUntil(1, std::chrono::seconds(1)), 1u);
    EXPECT_EQ(
        probe_->CountByInterfaceId(
            static_cast<uint32_t>(IpcCommandType::RemoteAddRef)),
        1u);
    EXPECT_EQ(run_loop_->GetPendingAddRefCount(FORK_PEER_SESSION_ID), 0u);

    materialized_proxy.Reset();
}

// 非 BT 线程（测试主线程，传真实 BT 的 weak_ptr）物化：候车语义与 BT
// 同款入队零直发；显式出清后帧在窗口内到达，单帧独占一次物理写
TEST_F(
    ProxyAddRefForkTest,
    NonBusinessThreadMaterializationDeliversViaExplicitFlush)
{
    const auto physical_writes_before =
        run_loop_->GetOutboundPhysicalWriteCount();

    auto [result, proxy] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(proxy);

    // 零载体场景：显式出清是候车控制面的唯一确定性送出机制
    static_cast<void>(run_loop_->FlushOutboundFifo());

    EXPECT_EQ(probe_->DrainUntil(1, std::chrono::seconds(1)), 1u);
    EXPECT_EQ(
        probe_->CountByInterfaceId(
            static_cast<uint32_t>(IpcCommandType::RemoteAddRef)),
        1u);
    EXPECT_EQ(
        run_loop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        1u);
    EXPECT_EQ(run_loop_->GetPendingAddRefCount(FORK_PEER_SESSION_ID), 0u);
    EXPECT_TRUE(run_loop_->PendingAddRefsEmpty());

    proxy.Reset();
}

// T10 完整锚点：非 BT 线程物化 → ①显式出清后窗口内一条独立 ADD_REF 帧
// 且 header 无 bit1；②FIFO 前置为空、帧到达后出清为空；③该帧 target =
// oid.session_id
TEST_F(
    ProxyAddRefForkTest,
    ExternalThreadMaterializationDeliversStandaloneFrameViaExplicitFlush)
{
    EXPECT_TRUE(run_loop_->PendingAddRefsEmpty());
    const auto physical_writes_before =
        run_loop_->GetOutboundPhysicalWriteCount();

    auto [result, proxy] = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(proxy);

    // 零载体场景：显式出清送出入队帧，不待周期/心跳班车
    static_cast<void>(run_loop_->FlushOutboundFifo());

    // ① 出清后当次窗口内到达一条独立 ADD_REF
    EXPECT_EQ(probe_->DrainUntil(1, std::chrono::seconds(1)), 1u);
    EXPECT_EQ(
        run_loop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        1u);
    const auto add_ref_frames = probe_->FramesByInterfaceId(
        static_cast<uint32_t>(IpcCommandType::RemoteAddRef));
    ASSERT_EQ(add_ref_frames.size(), 1u);
    EXPECT_EQ(
        add_ref_frames[0]->header.GetMessageType(),
        DAS::Core::IPC::MessageType::Event);
    EXPECT_EQ(
        add_ref_frames[0]->header.GetHeaderFlags(),
        DAS::Core::IPC::HeaderFlags::CONTROL_PLANE);
    // 独立帧形态：无 RESERVED_AS_SIZE（bit1）
    EXPECT_EQ(
        add_ref_frames[0]->header.GetFlags()
            & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
        0);
    // ③ target = oid.session_id（owner）+ body 为 ObjectId 字节
    EXPECT_EQ(
        add_ref_frames[0]->header.GetTargetSessionId(),
        object_id_.session_id);
    ASSERT_EQ(add_ref_frames[0]->body.size(), sizeof(ObjectId));
    EXPECT_EQ(
        std::memcmp(
            add_ref_frames[0]->body.data(),
            &object_id_,
            sizeof(ObjectId)),
        0);

    // ② FIFO 前置为空、帧到达后出清为空（前置 + 后置双侧观测）
    EXPECT_TRUE(run_loop_->PendingAddRefsEmpty());

    proxy.Reset();
}

// 仅 newly_created 触发：同对象第二接口视图（QI 衍生同 oid）既不入队也
// 不直发
TEST_F(ProxyAddRefForkTest, SecondInterfaceViewNeitherEnqueuesNorDirectSends)
{
    DasPtr<IDasBase>    first_proxy;
    DasPtr<IDasBase>    second_proxy;
    std::atomic<size_t> pending_count_at_materialization{0};
    RunOnBusinessThread(
        [&]()
        {
            auto [first_result, string_proxy] = proxy_factory_.GetOrCreateProxy(
                *run_loop_,
                business_thread_,
                object_id_,
                DasReadOnlyStringProxy::InterfaceId);
            ASSERT_EQ(first_result, DAS_S_OK);
            ASSERT_TRUE(string_proxy);
            first_proxy = std::move(string_proxy);

            auto [second_result, vector_proxy] =
                proxy_factory_.GetOrCreateProxy(
                    *run_loop_,
                    business_thread_,
                    object_id_,
                    DasVariantVectorByValueProxy::InterfaceId);
            ASSERT_EQ(second_result, DAS_S_OK);
            ASSERT_TRUE(vector_proxy);
            // 两视图物化后、周期结束前：pending 仍只有首视图的一条——
            // 第二视图（QI 衍生同 oid）既不入队也不直发
            pending_count_at_materialization.store(
                run_loop_->GetPendingAddRefCount(FORK_PEER_SESSION_ID),
                std::memory_order_release);
            second_proxy = std::move(vector_proxy);
        });

    EXPECT_EQ(
        pending_count_at_materialization.load(std::memory_order_acquire),
        1u);

    // 周期末出清：全周期恰一条 ADD_REF（首视图），第二视图零追加
    EXPECT_EQ(probe_->DrainUntil(1, std::chrono::seconds(1)), 1u);
    EXPECT_EQ(
        probe_->CountByInterfaceId(
            static_cast<uint32_t>(IpcCommandType::RemoteAddRef)),
        1u);
    EXPECT_EQ(probe_->DrainFor(std::chrono::milliseconds(300)), 0u);
    EXPECT_EQ(run_loop_->GetPendingAddRefCount(FORK_PEER_SESSION_ID), 0u);

    second_proxy.Reset();
    first_proxy.Reset();
}

// piggyback 总闸关闭：off = drain 不合写逐帧发——帧仍全部经 FIFO 入队
// （禁止旁路直发重新引入双写者），物化帧在窗口内到达且独占一次物理写
// （A/B 基线行为）
TEST_F(
    ProxyAddRefForkTest,
    DisabledPiggybackFlushesBusinessThreadMaterializationFrameByFrame)
{
    run_loop_->SetPiggybackEnabled(false);
    const auto physical_writes_before =
        run_loop_->GetOutboundPhysicalWriteCount();

    DasPtr<IDasBase> materialized_proxy;
    RunOnBusinessThread(
        [&]()
        {
            auto [result, proxy] = proxy_factory_.GetOrCreateProxy(
                *run_loop_,
                business_thread_,
                object_id_,
                DasReadOnlyStringProxy::InterfaceId);
            ASSERT_EQ(result, DAS_S_OK);
            ASSERT_TRUE(proxy);
            materialized_proxy = std::move(proxy);
        });

    EXPECT_EQ(probe_->DrainUntil(1, std::chrono::seconds(1)), 1u);
    EXPECT_EQ(
        probe_->CountByInterfaceId(
            static_cast<uint32_t>(IpcCommandType::RemoteAddRef)),
        1u);
    EXPECT_EQ(
        run_loop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        1u);
    EXPECT_TRUE(run_loop_->PendingAddRefsEmpty());

    materialized_proxy.Reset();
}

// ====== 闸3 周期末出清（AddRefDispatchGuard 挂点）======

// T9' BT 漏斗：BT 上物化入队后本 dispatch 周期无任何出站载体 →
// ProcessInboundMessage 退出（guard 析构）时 pending 已被独立发出——
// 本装置 run loop 无心跳线程，帧能在当次窗口到达只可能是周期末出清
// （时刻远早于心跳间隔）
TEST_F(ProxyAddRefForkTest, BusinessThreadCycleEndFlushesPendingWithoutCarrier)
{
    const auto physical_writes_before =
        run_loop_->GetOutboundPhysicalWriteCount();

    DasPtr<IDasBase> materialized_proxy;
    RunOnBusinessThread(
        [&]()
        {
            auto [result, proxy] = proxy_factory_.GetOrCreateProxy(
                *run_loop_,
                business_thread_,
                object_id_,
                DasReadOnlyStringProxy::InterfaceId);
            ASSERT_EQ(result, DAS_S_OK);
            ASSERT_TRUE(proxy);
            materialized_proxy = std::move(proxy);
        });

    // 入队后本周期零出站帧；周期末 guard 兜底触发 drain——滞留帧以合写
    // 批出清（单帧批一次物理写、一逻辑帧）
    EXPECT_EQ(probe_->DrainUntil(1, std::chrono::seconds(1)), 1u);
    EXPECT_EQ(
        probe_->CountByInterfaceId(
            static_cast<uint32_t>(IpcCommandType::RemoteAddRef)),
        1u);
    EXPECT_EQ(
        run_loop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        1u);
    EXPECT_EQ(run_loop_->GetPendingAddRefCount(FORK_PEER_SESSION_ID), 0u);
    EXPECT_TRUE(run_loop_->PendingAddRefsEmpty());

    // 合写批内帧形态：无 bit1（pipe 连写不置位）
    const auto add_ref_frames = probe_->FramesByInterfaceId(
        static_cast<uint32_t>(IpcCommandType::RemoteAddRef));
    ASSERT_EQ(add_ref_frames.size(), 1u);
    EXPECT_EQ(
        add_ref_frames[0]->header.GetFlags()
            & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
        0);

    materialized_proxy.Reset();
}

// T9' 队列不跨周期：一次完整入站 dispatch 周期（BT 物化 + 响应发出）→
// 周期末断言队列空（guard 忘挂在此暴露为可执行失败）
namespace
{
    constexpr uint32_t MATERIALIZING_HANDLER_INTERFACE_ID = 0x7777;

    // 周期内物化（BT 入队）并发响应的测试 handler
    class MaterializingRequestHandler final
        : public DAS::Core::IPC::IMessageHandler
    {
    public:
        MaterializingRequestHandler(
            ProxyFactory&                 proxy_factory,
            IpcRunLoop&                   run_loop,
            std::weak_ptr<BusinessThread> business_thread,
            const ObjectId&               object_id,
            DasPtr<IDasBase>*             materialized_proxy_slot)
            : proxy_factory_(proxy_factory), run_loop_(run_loop),
              business_thread_(std::move(business_thread)),
              object_id_(object_id),
              materialized_proxy_slot_(materialized_proxy_slot)
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
            return MATERIALIZING_HANDLER_INTERFACE_ID;
        }

        DasResult HandleMessage(
            const ValidatedIPCMessageHeader&   header,
            const std::vector<uint8_t>&        body,
            DAS::Core::IPC::IpcResponseSender& sender,
            DAS::Core::IPC::StubContext&       resolve_context) override
        {
            (void)body;
            (void)resolve_context;

            // BT 分发线程上物化 → 入队候车（proxy 跨周期存活，不在
            // 周期内析构触发 RELEASE）
            auto [result, proxy] = proxy_factory_.GetOrCreateProxy(
                run_loop_,
                business_thread_,
                object_id_,
                DasReadOnlyStringProxy::InterfaceId);
            if (DAS::IsFailed(result) || !proxy)
            {
                return result;
            }
            *materialized_proxy_slot_ = std::move(proxy);

            // 周期中段发出响应（业务出站帧，RESPONSE source/target 互换）
            const auto response_header =
                IPCMessageHeaderBuilder()
                    .SetMessageType(MessageType::Response)
                    .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::NONE)
                    .SetInterfaceId(header.GetInterfaceId())
                    .SetCallId(header.GetCallId())
                    .SetSourceSessionId(header.GetTargetSessionId())
                    .SetTargetSessionId(header.GetSourceSessionId())
                    .SetBodySize(0)
                    .Build();
            const std::vector<uint8_t> empty_body;
            return sender.SendResponse(response_header, empty_body);
        }

    private:
        std::atomic<uint32_t>         ref_count_{0};
        ProxyFactory&                 proxy_factory_;
        IpcRunLoop&                   run_loop_;
        std::weak_ptr<BusinessThread> business_thread_;
        const ObjectId&               object_id_;
        DasPtr<IDasBase>*             materialized_proxy_slot_;
    };
} // namespace

TEST_F(
    ProxyAddRefForkTest,
    InboundDispatchCycleEndFlushesPendingBeforeReturning)
{
    // 物化的 proxy 跨周期存活（真实 [in] 参数流形态），断言完成后释放
    DasPtr<IDasBase> materialized_proxy;

    // 堆分配（RegisterHandler 的 DasPtr 持有并在 run loop 析构时释放）
    auto* materializing_handler = new MaterializingRequestHandler(
        proxy_factory_,
        *run_loop_,
        business_thread_,
        object_id_,
        &materialized_proxy);
    run_loop_->RegisterHandler(
        DAS::Core::IPC::HeaderFlags::NONE,
        MATERIALIZING_HANDLER_INTERFACE_ID,
        materializing_handler);

    const auto physical_writes_before =
        run_loop_->GetOutboundPhysicalWriteCount();

    // 投递 REQUEST 入站 → BT 拾起 → 完整 dispatch 周期（物化 + 响应）
    DAS::Core::IPC::InboundMessage request;
    request.header = IPCMessageHeaderBuilder()
                         .SetMessageType(MessageType::Request)
                         .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::NONE)
                         .SetInterfaceId(MATERIALIZING_HANDLER_INTERFACE_ID)
                         .SetCallId(1)
                         .SetSourceSessionId(FORK_PEER_SESSION_ID)
                         .SetTargetSessionId(FORK_LOCAL_SESSION_ID)
                         .Build();
    ASSERT_EQ(inbound_queue_->Push(std::move(request)), DAS_S_OK);

    // 出站 FIFO 语义：BT 物化的 130 入队不触发，同周期 Response post 即
    // 触发 drain——两帧同批连写一次物理写，wire 序 [130][Response] =
    // 入队序 = 事件序；周期末 guard 对已出清队列零动作，队列不跨周期
    EXPECT_EQ(probe_->DrainUntil(2, std::chrono::seconds(2)), 2u);
    ASSERT_EQ(probe_->TotalFrameCount(), 2u);
    EXPECT_EQ(
        probe_->InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(IpcCommandType::RemoteAddRef),
            MATERIALIZING_HANDLER_INTERFACE_ID}));

    const auto& response_frame = probe_->Frames()[1];
    EXPECT_EQ(
        response_frame.header.GetInterfaceId(),
        MATERIALIZING_HANDLER_INTERFACE_ID);

    // 连写形态：两逻辑帧恰一次物理写、全帧无 bit1（pipe 连写不置位）
    EXPECT_EQ(
        run_loop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        1u);
    for (const auto& wire_frame : probe_->Frames())
    {
        EXPECT_EQ(
            wire_frame.header.GetFlags()
                & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
            0);
    }

    // 守恒：恰一次 ADD_REF 申报到达
    EXPECT_EQ(
        probe_->CountByInterfaceId(
            static_cast<uint32_t>(IpcCommandType::RemoteAddRef)),
        1u);

    // 队列不跨周期：周期末已出清
    EXPECT_EQ(run_loop_->GetPendingAddRefCount(FORK_PEER_SESSION_ID), 0u);
    EXPECT_TRUE(run_loop_->PendingAddRefsEmpty());

    materialized_proxy.Reset();
}

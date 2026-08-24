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
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/DasReadOnlyStringProxy.h>
#include <das/Core/IPC/DasVariantVectorByValueProxy.h>
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
#include <optional>
#include <thread>
#include <utility>

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
using DAS::Core::IPC::IPCProxyBase;
using DAS::Core::IPC::IpcRunLoop;
using DAS::Core::IPC::IpcRuntimeState;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::ProxyFactory;
using DAS::Core::IPC::RemoteObjectRegistry;

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
    IpcRunLoop           run_loop(false, nullptr, proxy_factory, registry);
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
        run_loop_ = std::make_unique<IpcRunLoop>(
            false,
            nullptr,
            proxy_factory_,
            registry_);
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
        run_loop_ = std::make_unique<IpcRunLoop>(
            false,
            nullptr,
            proxy_factory_,
            registry_);
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

    // 恰好一条：短期内不得出现第二条申报
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

    auto message = ReceiveNextSentMessage(std::chrono::seconds(1));
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(
        message->first.GetInterfaceId(),
        static_cast<uint32_t>(IpcCommandType::RemoteAddRef));

    // 第二视图物化后不得再有追加申报
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

    // 软排空物化期报文，之后的线上流量只剩 RELEASE
    static_cast<void>(ReceiveNextSentMessage(std::chrono::seconds(1)));

    vector_proxy.Reset();
    string_proxy.Reset();

    auto release_message = ReceiveNextSentMessage(std::chrono::seconds(1));
    ASSERT_TRUE(release_message.has_value())
        << "entry removal must send exactly one RemoteRelease";

    const auto& release_header = release_message->first;
    EXPECT_EQ(release_header.GetMessageType(), MessageType::Event);
    EXPECT_EQ(
        release_header.GetHeaderFlags(),
        DAS::Core::IPC::HeaderFlags::CONTROL_PLANE);
    EXPECT_EQ(
        release_header.GetInterfaceId(),
        static_cast<uint32_t>(IpcCommandType::RemoteRelease));
    EXPECT_EQ(release_header.GetSourceSessionId(), run_loop_->GetSessionId());
    EXPECT_EQ(release_header.GetTargetSessionId(), object_id_.session_id);
    ASSERT_EQ(release_message->second.size(), sizeof(ObjectId));
    EXPECT_EQ(
        std::memcmp(
            release_message->second.data(),
            &object_id_,
            sizeof(ObjectId)),
        0);

    // 恰好一条：条目只移除一次，不得按 proxy 实例数多发
    EXPECT_FALSE(
        ReceiveNextSentMessage(std::chrono::milliseconds(300)).has_value());
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

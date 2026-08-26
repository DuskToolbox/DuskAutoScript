#include <atomic>
#include <chrono>
#include <cstdint>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/DasReadOnlyStringProxy.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IHostConnection.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/DasPtr.hpp>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>

#include "MockDasObject.h"
#include "OutboundFrameProbe.h"
#include "TestTransportResolvers.h"

using DAS::Core::IPC::DasReadOnlyStringProxy;
using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::IpcRunLoop;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::ProxyFactory;
using DAS::Core::IPC::RemoteObjectInfo;
using DAS::Core::IPC::RemoteObjectRegistry;
using DAS::Core::IPC::TransportLookupResult;

namespace
{
    DasGuid MakeThreadingGuid(uint32_t seed)
    {
        return DasGuid{
            .data1 = 0x67010000u + seed,
            .data2 = static_cast<uint16_t>(0x1000u + seed),
            .data3 = static_cast<uint16_t>(0x2000u + seed),
            .data4 = {
                static_cast<uint8_t>(seed),
                0x67,
                0x01,
                0x54,
                0x48,
                0x52,
                0x44,
                static_cast<uint8_t>(0x80u + (seed & 0x7Fu))}};
    }

    class ReentrantReleaseObject final : public IDasBase
    {
    public:
        explicit ReentrantReleaseObject(DistributedObjectManager& manager)
            : manager_(manager)
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                callback_entered_.store(true, std::memory_order_release);
                static_cast<void>(manager_.IsValidObject(
                    ObjectId{
                        .session_id = 42,
                        .generation = 1,
                        .local_id = 99}));
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL QueryInterface(const DasGuid&, void**) override
        {
            return DAS_E_NO_INTERFACE;
        }

        static bool CallbackEntered() noexcept
        {
            return callback_entered_.load(std::memory_order_acquire);
        }

    private:
        ~ReentrantReleaseObject() = default;

        DistributedObjectManager& manager_;
        std::atomic<uint32_t>     ref_count_{0};

        static std::atomic<bool> callback_entered_;
    };

    std::atomic<bool> ReentrantReleaseObject::callback_entered_{false};
} // namespace

TEST(IpcRuntimeThreadingTest, RemoteObjectRegistryConcurrentCopyOut)
{
    RemoteObjectRegistry     registry;
    std::atomic<bool>        start{false};
    std::vector<std::thread> threads;

    constexpr int writer_count = 4;
    constexpr int objects_per_writer = 64;

    for (int writer = 0; writer < writer_count; ++writer)
    {
        threads.emplace_back(
            [&, writer]
            {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }

                for (int i = 0; i < objects_per_writer; ++i)
                {
                    const auto local_id = static_cast<uint32_t>(
                        writer * objects_per_writer + i + 1);
                    const ObjectId object_id{
                        .session_id = static_cast<uint16_t>(writer + 1),
                        .generation = 1,
                        .local_id = local_id};
                    const auto interface_id = 0x67010000u + local_id;
                    ASSERT_EQ(
                        registry.RegisterObject(
                            object_id,
                            MakeThreadingGuid(local_id),
                            interface_id,
                            object_id.session_id,
                            "threaded-object-" + std::to_string(local_id),
                            1),
                        DAS_S_OK);

                    RemoteObjectInfo info{};
                    const auto by_id = registry.GetObjectInfo(object_id, info);
                    EXPECT_TRUE(
                        by_id == DAS_S_OK
                        || by_id == DAS_E_IPC_OBJECT_NOT_FOUND);
                    if (by_id == DAS_S_OK)
                    {
                        EXPECT_EQ(info.object_id, object_id);
                        EXPECT_FALSE(info.name.empty());
                    }
                }
            });
    }

    for (int reader = 0; reader < 4; ++reader)
    {
        threads.emplace_back(
            [&]
            {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }

                for (int i = 0; i < writer_count * objects_per_writer; ++i)
                {
                    std::vector<RemoteObjectInfo> objects;
                    registry.ListAllObjects(objects);
                    for (const auto& object : objects)
                    {
                        EXPECT_FALSE(object.name.empty());
                        EXPECT_NE(object.interface_id, 0u);
                    }
                }
            });
    }

    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(
        registry.GetObjectCount(),
        static_cast<size_t>(writer_count * objects_per_writer));
}

TEST(
    IpcRuntimeThreadingTest,
    RemoteObjectRegistryClearAndSessionRemovalAreLocked)
{
    RemoteObjectRegistry registry;
    std::atomic<bool>    stop{false};

    std::thread writer(
        [&]
        {
            for (uint32_t i = 1; i <= 128; ++i)
            {
                const ObjectId object_id{
                    .session_id = static_cast<uint16_t>((i % 3) + 1),
                    .generation = 1,
                    .local_id = i};
                static_cast<void>(registry.RegisterObject(
                    object_id,
                    MakeThreadingGuid(i),
                    0x67020000u + i,
                    object_id.session_id,
                    "session-object-" + std::to_string(i),
                    1));
                if ((i % 17) == 0)
                {
                    registry.UnregisterAllFromSession(2);
                }
                if ((i % 43) == 0)
                {
                    registry.Clear();
                }
            }
            stop.store(true, std::memory_order_release);
        });

    while (!stop.load(std::memory_order_acquire))
    {
        std::vector<RemoteObjectInfo> objects;
        registry.ListObjectsBySession(1, objects);
        for (const auto& object : objects)
        {
            EXPECT_EQ(object.session_id, 1);
        }
    }

    writer.join();
}

TEST(
    IpcRuntimeThreadingTest,
    DistributedObjectManagerSessionAndRemoteRefcountAreThreadSafe)
{
    DistributedObjectManager manager;
    std::atomic<bool>        start{false};
    std::vector<std::thread> threads;

    for (uint16_t session = 1; session <= 4; ++session)
    {
        threads.emplace_back(
            [&, session]
            {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                for (int i = 0; i < 128; ++i)
                {
                    manager.SetSessionId(session);
                    ObjectId id{};
                    auto*    object = new MockDasObject();
                    ASSERT_EQ(
                        manager.RegisterLocalObject(object, id),
                        DAS_S_OK);
                    EXPECT_NE(id.session_id, 0);
                    ASSERT_EQ(manager.UnregisterObject(id), DAS_S_OK);
                }
            });
    }

    const ObjectId remote_id{.session_id = 99, .generation = 1, .local_id = 7};
    threads.emplace_back(
        [&]
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            for (int i = 0; i < 64; ++i)
            {
                ASSERT_EQ(manager.RegisterRemoteObject(remote_id), DAS_S_OK);
            }
            for (int i = 0; i < 63; ++i)
            {
                ASSERT_EQ(manager.UnregisterObject(remote_id), DAS_S_OK);
                EXPECT_TRUE(manager.IsValidObject(remote_id));
            }
            ASSERT_EQ(manager.UnregisterObject(remote_id), DAS_S_OK);
            EXPECT_FALSE(manager.IsValidObject(remote_id));
        });

    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
    {
        thread.join();
    }
}

TEST(IpcRuntimeThreadingTest, DistributedObjectManagerFinalReleaseIsOutsideLock)
{
    DistributedObjectManager manager;
    ObjectId                 id{};
    auto*                    object = new ReentrantReleaseObject(manager);

    ASSERT_EQ(manager.RegisterLocalObject(object, id), DAS_S_OK);
    ASSERT_EQ(manager.RegisterLocalObject(object, id), DAS_S_OK);

    ASSERT_EQ(manager.UnregisterObject(id), DAS_S_OK);
    EXPECT_TRUE(manager.IsValidObject(id));
    EXPECT_FALSE(ReentrantReleaseObject::CallbackEntered());

    ASSERT_EQ(manager.UnregisterObject(id), DAS_S_OK);
    EXPECT_FALSE(manager.IsValidObject(id));
    EXPECT_TRUE(ReentrantReleaseObject::CallbackEntered());
}

TEST(
    IpcRuntimeThreadingTest,
    LateProxyReleaseAfterRuntimeTokenExpiredSkipsRuntimeTables)
{
    auto                 proxy_factory = std::make_unique<ProxyFactory>();
    RemoteObjectRegistry registry;
    std::unique_ptr<IpcRunLoop> run_loop;
    // main 语义构造点：显式传两级查表 resolver
    run_loop = std::make_unique<IpcRunLoop>(
        false,
        nullptr,
        *proxy_factory,
        registry,
        [&run_loop](uint16_t target_session_id) -> TransportLookupResult
        {
            return ResolveTransportMainSemantic(
                run_loop->GetConnectionManager(),
                target_session_id);
        });

    auto* proxy = new DasReadOnlyStringProxy(
        DasReadOnlyStringProxy::InterfaceId,
        ObjectId{.session_id = 9, .generation = 1, .local_id = 44},
        *run_loop,
        {},
        *proxy_factory);

    proxy_factory->BeginShutdown();
    run_loop.reset();
    proxy_factory.reset();

    EXPECT_EQ(proxy->Release(), 0u);
}

TEST(IpcRuntimeThreadingTest, RemoteHolderLedgerConcurrentAccessIsClean)
{
    DistributedObjectManager manager;
    manager.SetSessionId(1);

    constexpr int         object_count = 24;
    constexpr uint16_t    session_count = 4;
    std::vector<ObjectId> object_ids;
    for (int index = 0; index < object_count; ++index)
    {
        auto*    mock = new MockDasObject();
        ObjectId object_id{};
        ASSERT_EQ(
            manager.RegisterLocalObject(mock, object_id, false),
            DAS_S_OK);
        object_ids.push_back(object_id);
    }

    std::atomic<bool>        start{false};
    std::vector<std::thread> threads;

    // IO 记账写者：AddRemoteHolder / ReleaseRemoteHolder 混跑
    for (int writer = 0; writer < 2; ++writer)
    {
        threads.emplace_back(
            [&, writer]
            {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                for (int index = 0; index < 600; ++index)
                {
                    const auto& object_id =
                        object_ids[(index + writer) % object_count];
                    const auto session_id = static_cast<uint16_t>(
                        (index / object_count) % session_count + 1);

                    const auto add_result =
                        manager.AddRemoteHolder(object_id, session_id);
                    EXPECT_TRUE(
                        add_result == DAS_S_OK
                        || add_result == DAS_E_IPC_OBJECT_NOT_FOUND);

                    DAS::DasPtr<IDasBase> released;
                    const auto release_result = manager.ReleaseRemoteHolder(
                        object_id,
                        session_id,
                        released);
                    EXPECT_TRUE(
                        release_result == DAS_S_OK
                        || release_result == DAS_E_IPC_OBJECT_NOT_FOUND);
                }
            });
    }

    // 断连清扫者：RemoveSessionHolders 与记账混跑（出参对象本体集合的
    // 析构线程域由调用方决定——此处立即析构模拟 BT 侧消费）
    threads.emplace_back(
        [&]
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            for (int index = 0; index < 300; ++index)
            {
                const auto removed_objects = manager.RemoveSessionHolders(
                    static_cast<uint16_t>(index % session_count + 1));
                (void)removed_objects;
            }
        });

    // BT 侧读者：WithLocalObject 与记账/清扫并发
    threads.emplace_back(
        [&]
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            for (int index = 0; index < 600; ++index)
            {
                const auto& object_id = object_ids[index % object_count];
                static_cast<void>(manager.WithLocalObject(
                    object_id,
                    [](DAS::Core::IPC::LocalObjectRef& ref)
                    {
                        return ref.Get() != nullptr ? DAS_S_OK
                                                    : DAS_E_IPC_INVALID_STATE;
                    }));
            }
        });

    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
    {
        thread.join();
    }

    // 终态一致性：全 session 清扫后，无 pin 条目全部回收
    for (uint16_t session_id = 1; session_id <= session_count; ++session_id)
    {
        static_cast<void>(manager.RemoveSessionHolders(session_id));
    }
    EXPECT_EQ(manager.GetLocalEntryCount(), 0u);
}

// 出站 FIFO 并发（Pitfall 11）：多线程 EnqueueAddRef / FlushOutboundFifo /
// ClearOutboundFifo 与非 BT 物化（入队即触发 drain）混跑——无数据竞争；
// 守恒断言钉死每帧恰出队一次（drain 写出或清除，二者计数之和恒等入队
// 总数——重复出队或丢帧都会破坏等式）。drain 消费数经逻辑帧计数观测
// （每 session 挂真实 pipe，写出全部成功）
TEST(
    IpcRuntimeThreadingTest,
    PendingAddRefQueueConcurrentEnqueueFlushClearIsClean)
{
    using DAS::Core::IPC::InboundMessage;
    using DAS::Core::IPC::IpcMessageQueue;
    using DAS::Core::IPC::IpcRunLoop;
    using DAS::Core::IPC::ObjectId;
    using DAS::Core::IPC::ProxyFactory;
    using DAS::Core::IPC::RemoteObjectRegistry;

    ProxyFactory                    proxy_factory;
    RemoteObjectRegistry            registry;
    IpcMessageQueue<InboundMessage> inbound_queue(256);
    // main 语义构造点：显式传两级查表 resolver
    IpcRunLoop run_loop(
        false,
        &inbound_queue,
        proxy_factory,
        registry,
        [&run_loop](uint16_t target_session_id) -> TransportLookupResult
        {
            return ResolveTransportMainSemantic(
                run_loop.GetConnectionManager(),
                target_session_id);
        });
    run_loop.SetSessionId(1);

    constexpr uint16_t cleared_session_id = 2;
    const ObjectId     object_ids[3] = {
        ObjectId{.session_id = 2, .generation = 1, .local_id = 1},
        ObjectId{.session_id = 3, .generation = 1, .local_id = 2},
        ObjectId{.session_id = 4, .generation = 1, .local_id = 3}};

    // 三个 session 各挂一条真实 pipe（io 线程启动前注册）：drain 写出
    // 可达——逻辑帧计数即 drain 消费数的可观测面（守恒方程的第二项）
    std::vector<std::unique_ptr<OutboundFrameProbe>> session_probes;
    for (const ObjectId& object_id : object_ids)
    {
        auto probe =
            std::make_unique<OutboundFrameProbe>(run_loop.GetIoContext(), true);
        ASSERT_TRUE(probe->IsHealthy());
        auto counterpart_transport = probe->TakeCounterpartTransport();
        ASSERT_TRUE(counterpart_transport.has_value());
        ASSERT_EQ(
            run_loop.GetConnectionManager().RegisterHostLocalTransport(
                object_id.session_id,
                std::move(*counterpart_transport)),
            DAS_S_OK);
        session_probes.push_back(std::move(probe));
    }
    const auto logical_frames_before = run_loop.GetOutboundLogicalFrameCount();

    // io 线程持续驱动 drain 协程（外部驱动模式，与生产拓扑一致）
    auto work_guard = boost::asio::make_work_guard(run_loop.GetIoContext());
    std::thread io_thread([&run_loop]() { run_loop.GetIoContext().run(); });

    constexpr int enqueue_iterations = 300;
    constexpr int materialize_iterations = 50;

    std::atomic<bool>   start{false};
    std::atomic<size_t> enqueued_total{0};
    std::atomic<size_t> cleared_total{0};

    std::vector<std::thread> threads;

    // 入队者 ×2：跨三个 session 轮转入队（入队不触发——滞留由出清/清除
    // /drain 触发者消费）
    for (int enqueuer = 0; enqueuer < 2; ++enqueuer)
    {
        threads.emplace_back(
            [&, enqueuer]
            {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                for (int index = 0; index < enqueue_iterations; ++index)
                {
                    run_loop.EnqueueAddRef(object_ids[(index + enqueuer) % 3]);
                    enqueued_total.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    // 出清者：反复触发整体 drain（消费数并入逻辑帧计数，不单独累计——
    // FlushOutboundFifo 返回触发时点滞留数，非消费数）
    threads.emplace_back(
        [&]
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            for (int index = 0; index < 200; ++index)
            {
                static_cast<void>(run_loop.FlushOutboundFifo());
            }
        });

    // 清队者：反复清除 session 2（部分帧按断连语义被丢弃——计数守恒）
    threads.emplace_back(
        [&]
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            for (int index = 0; index < 200; ++index)
            {
                cleared_total.fetch_add(
                    run_loop.ClearOutboundFifo(cleared_session_id),
                    std::memory_order_relaxed);
            }
        });

    // 物化者：非 BT 线程物化（入队即触发 drain，与队列操作混跑——
    // proxy cache 与 DOM 锁路径的并发回归）；每迭代物化+死亡恰 2 帧
    // （ADD_REF + RELEASE）额外进入 FIFO
    threads.emplace_back(
        [&]
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            for (int index = 0; index < materialize_iterations; ++index)
            {
                auto [result, proxy] = proxy_factory.GetOrCreateProxy(
                    run_loop,
                    {},
                    object_ids[0],
                    DAS::Core::IPC::DasReadOnlyStringProxy::InterfaceId);
                if (!DAS::IsFailed(result) && proxy)
                {
                    proxy.Reset();
                }
            }
        });

    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    // 终末出清：触发全部残留 session 的 drain，等 FIFO 排空且在途写
    // 完成（逻辑帧计数稳定）
    static_cast<void>(run_loop.FlushOutboundFifo());
    bool queue_drained = false;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        if (run_loop.PendingAddRefsEmpty())
        {
            queue_drained = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(queue_drained) << "outbound FIFO must drain after final flush";
    auto settled_logical_frames = run_loop.GetOutboundLogicalFrameCount();
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto latest_logical_frames =
            run_loop.GetOutboundLogicalFrameCount();
        if (latest_logical_frames == settled_logical_frames)
        {
            break;
        }
        settled_logical_frames = latest_logical_frames;
    }
    work_guard.reset();
    io_thread.join();

    // 守恒成立：每帧恰出队一次（drain 写出 + 清除 == 入队总数；物化者
    // 每迭代额外贡献 ADD_REF/RELEASE 两帧）
    EXPECT_EQ(
        enqueued_total.load() + 2 * static_cast<size_t>(materialize_iterations),
        (settled_logical_frames - logical_frames_before)
            + cleared_total.load());
    EXPECT_TRUE(run_loop.PendingAddRefsEmpty());

    proxy_factory.BeginShutdown();
}

#include <atomic>
#include <cstdint>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "MockDasObject.h"

using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::RemoteObjectInfo;
using DAS::Core::IPC::RemoteObjectRegistry;

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

    constexpr int kWriterCount = 4;
    constexpr int kObjectsPerWriter = 64;

    for (int writer = 0; writer < kWriterCount; ++writer)
    {
        threads.emplace_back(
            [&, writer]
            {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }

                for (int i = 0; i < kObjectsPerWriter; ++i)
                {
                    const auto local_id = static_cast<uint32_t>(
                        writer * kObjectsPerWriter + i + 1);
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

                for (int i = 0; i < kWriterCount * kObjectsPerWriter; ++i)
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
        static_cast<size_t>(kWriterCount * kObjectsPerWriter));
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

#include <chrono>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

using DAS::Core::IPC::InboundMessage;
using DAS::Core::IPC::IpcMessageQueue;

// Test fixture for IpcMessageQueue tests
class IpcMessageQueueTest : public ::testing::Test
{
};

// ====== Basic Push/Pop Tests ======

TEST_F(IpcMessageQueueTest, Push_NonFullQueue_ReturnsSuccess)
{
    IpcMessageQueue<int> queue(10);

    auto result = queue.Push(42);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcMessageQueueTest, Push_Pop_ReturnsSameItem)
{
    IpcMessageQueue<int> queue(10);

    queue.Push(42);
    auto item = queue.Pop();

    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item.value(), 42);
}

TEST_F(IpcMessageQueueTest, Push_FullQueue_ReturnsQueueFull)
{
    IpcMessageQueue<int> queue(2);

    EXPECT_EQ(queue.Push(1), DAS_S_OK);
    EXPECT_EQ(queue.Push(2), DAS_S_OK);
    // Third push should fail because queue is full
    auto result = queue.Push(3);
    EXPECT_EQ(result, DAS_E_IPC_QUEUE_FULL);
}

TEST_F(IpcMessageQueueTest, Pop_BlocksUntilItemAvailable)
{
    IpcMessageQueue<int> queue(10);
    std::optional<int>   result;

    std::thread producer(
        [&queue]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            queue.Push(42);
        });

    // Pop should block until item is available
    auto item = queue.Pop();
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item.value(), 42);

    producer.join();
}

TEST_F(IpcMessageQueueTest, TryPop_EmptyQueue_ReturnsNullopt)
{
    IpcMessageQueue<int> queue(10);

    auto item = queue.TryPop();
    EXPECT_EQ(item, std::nullopt);
}

TEST_F(IpcMessageQueueTest, TryPop_NonEmptyQueue_ReturnsItem)
{
    IpcMessageQueue<int> queue(10);

    queue.Push(42);
    auto item = queue.TryPop();

    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item.value(), 42);
}

// ====== Shutdown Tests ======

TEST_F(IpcMessageQueueTest, Shutdown_PopReturnsNullopt)
{
    IpcMessageQueue<int> queue(10);
    queue.Shutdown();

    auto item = queue.Pop();
    EXPECT_EQ(item, std::nullopt);
}

TEST_F(IpcMessageQueueTest, Shutdown_PushReturnsCanceled)
{
    IpcMessageQueue<int> queue(10);
    queue.Shutdown();

    auto result = queue.Push(42);
    EXPECT_EQ(result, DAS_E_IPC_CANCELED);
}

TEST_F(IpcMessageQueueTest, Shutdown_IsShutdownReturnsTrue)
{
    IpcMessageQueue<int> queue(10);
    EXPECT_FALSE(queue.IsShutdown());

    queue.Shutdown();
    EXPECT_TRUE(queue.IsShutdown());
}

// ====== Concurrent Tests ======

TEST_F(IpcMessageQueueTest, MultipleProducersConsumers)
{
    IpcMessageQueue<int> queue(100);
    const int            num_items = 100;
    std::vector<int>     produced;
    std::vector<int>     consumed;
    std::mutex           produced_mutex;
    std::mutex           consumed_mutex;

    std::thread producer1(
        [&queue, num_items]()
        {
            for (int i = 0; i < num_items; ++i)
            {
                queue.Push(i);
            }
        });

    std::thread producer2(
        [&queue, num_items]()
        {
            for (int i = num_items; i < num_items * 2; ++i)
            {
                queue.Push(i);
            }
        });

    std::thread consumer(
        [&queue, &consumed, &consumed_mutex, num_items]()
        {
            for (int i = 0; i < num_items * 2; ++i)
            {
                auto item = queue.Pop();
                if (item.has_value())
                {
                    std::lock_guard<std::mutex> lock(consumed_mutex);
                    consumed.push_back(item.value());
                }
            }
        });

    producer1.join();
    producer2.join();
    queue.Shutdown();
    consumer.join();

    // All items should be consumed
    EXPECT_EQ(consumed.size(), static_cast<size_t>(num_items * 2));
}

// ====== InboundMessage Tests ======

TEST_F(IpcMessageQueueTest, InboundMessage_DefaultConstructs)
{
    InboundMessage msg;
    // Just verify it compiles and can be created
    EXPECT_TRUE(true);
}

TEST_F(IpcMessageQueueTest, InboundMessage_CanBePushedAndPopped)
{
    IpcMessageQueue<InboundMessage> queue(10);

    InboundMessage msg;
    msg.body = {1, 2, 3, 4, 5};

    queue.Push(std::move(msg));
    auto popped = queue.Pop();

    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(popped->body.size(), 5U);
    EXPECT_EQ(popped->body[0], 1);
    EXPECT_EQ(popped->body[4], 5);
}

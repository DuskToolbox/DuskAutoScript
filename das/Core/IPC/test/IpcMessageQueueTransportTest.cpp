#include <das/Core/IPC/MessageQueueTransport.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using DAS::Core::IPC::FromV1;
using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::IpcTransport;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::ToV1;

// Test fixture for MessageQueueTransport tests
class IpcMessageQueueTransportTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        transport_ = std::make_unique<IpcTransport>();
        // Generate unique queue names to avoid conflicts
        auto id = std::hash<std::thread::id>{}(std::this_thread::get_id());
        host_queue_name_ = "test_host_" + std::to_string(id);
        plugin_queue_name_ = "test_plugin_" + std::to_string(id);
    }

    void TearDown() override
    {
        if (transport_)
        {
            transport_->Shutdown();
        }
    }

    IPCMessageHeader CreateTestHeader(MessageType type = MessageType::REQUEST)
    {
        IPCMessageHeader header{};
        header.call_id = 1;
        header.message_type = type;
        header.error_code = DAS_S_OK;
        header.interface_id = 1;
        header.object_id = 0;
        header.version = 1;
        header.flags = 0;
        header.body_size = 0;
        return header;
    }

    std::unique_ptr<IpcTransport> transport_;
    std::string                   host_queue_name_;
    std::string                   plugin_queue_name_;
};

// ====== Initialize/Shutdown Tests ======

TEST_F(IpcMessageQueueTransportTest, Initialize_Succeeds)
{
    auto result =
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcMessageQueueTransportTest, Shutdown_Succeeds)
{
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10),
        DAS_S_OK);
    auto result = transport_->Shutdown();
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcMessageQueueTransportTest, IsConnected_AfterInitialize)
{
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10),
        DAS_S_OK);
    EXPECT_TRUE(transport_->IsConnected());
}

TEST_F(IpcMessageQueueTransportTest, IsConnected_AfterShutdown)
{
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10),
        DAS_S_OK);
    ASSERT_EQ(transport_->Shutdown(), DAS_S_OK);
    EXPECT_FALSE(transport_->IsConnected());
}

// ====== Small Message Tests (< 4KB) ======

TEST_F(IpcMessageQueueTransportTest, Send_SmallMessageSucceeds)
{
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10),
        DAS_S_OK);

    auto         header = CreateTestHeader();
    uint8_t      body[] = {1, 2, 3, 4, 5};
    const size_t body_size = sizeof(body);

    auto result = transport_->Send(header, body, body_size);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcMessageQueueTransportTest, Send_SmallMessageWithNullBody)
{
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10),
        DAS_S_OK);

    auto header = CreateTestHeader();
    header.body_size = 0;

    auto result = transport_->Send(header, nullptr, 0);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcMessageQueueTransportTest, Receive_SmallMessage)
{
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10),
        DAS_S_OK);

    auto         header = CreateTestHeader();
    uint8_t      body[] = {1, 2, 3, 4, 5};
    const size_t body_size = sizeof(body);
    header.body_size = static_cast<uint32_t>(body_size);

    ASSERT_EQ(transport_->Send(header, body, body_size), DAS_S_OK);

    IPCMessageHeader     recv_header;
    std::vector<uint8_t> recv_body;
    auto result = transport_->Receive(recv_header, recv_body, 1000);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(recv_header.call_id, header.call_id);
    EXPECT_EQ(recv_body.size(), body_size);
}

// ====== Large Message Tests (> 4KB) ======

TEST_F(IpcMessageQueueTransportTest, Send_LargeMessageRequiresSharedMemory)
{
    // Large message requires shared memory pool
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10),
        DAS_S_OK);

    auto                 header = CreateTestHeader();
    std::vector<uint8_t> large_body(8192, 0xAB); // 8KB
    header.body_size = static_cast<uint32_t>(large_body.size());

    // Without shared memory pool, should fail
    auto result =
        transport_->Send(header, large_body.data(), large_body.size());
    EXPECT_NE(result, DAS_S_OK); // Expected to fail without SHM pool
}

// ====== MakeQueueName Tests ======

TEST_F(IpcMessageQueueTransportTest, MakeQueueName_HostToPlugin)
{
    auto name = IpcTransport::MakeQueueName(1, 2, true);
    EXPECT_EQ(name, "das_ipc_1_2_h2p");
}

TEST_F(IpcMessageQueueTransportTest, MakeQueueName_PluginToHost)
{
    auto name = IpcTransport::MakeQueueName(1, 2, false);
    EXPECT_EQ(name, "das_ipc_1_2_p2h");
}

// ====== Error Cases ======

TEST_F(IpcMessageQueueTransportTest, Send_WithoutInitialize)
{
    auto    header = CreateTestHeader();
    uint8_t body[] = {1, 2, 3};

    auto result = transport_->Send(header, body, sizeof(body));
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(IpcMessageQueueTransportTest, Receive_WithoutInitialize)
{
    IPCMessageHeader     header;
    std::vector<uint8_t> body;

    auto result = transport_->Receive(header, body, 1000);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(IpcMessageQueueTransportTest, Receive_Timeout)
{
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10),
        DAS_S_OK);

    IPCMessageHeader     header;
    std::vector<uint8_t> body;

    // Receive with short timeout - should timeout since no message was sent
    auto result = transport_->Receive(header, body, 10);
    EXPECT_EQ(result, DAS_E_IPC_TIMEOUT);
}

// ====== Message Header Conversion Tests ======

TEST_F(IpcMessageQueueTransportTest, ToV1_FromV1_RoundTrip)
{
    IPCMessageHeader original = CreateTestHeader();
    original.call_id = 12345;
    original.error_code = -42;
    original.interface_id = 999;
    original.object_id = 0xDEADBEEF;
    original.flags = 0xFF;
    original.body_size = 1024;

    auto v1 = ToV1(original);
    auto restored = FromV1(v1);

    EXPECT_EQ(restored.call_id, original.call_id);
    EXPECT_EQ(restored.message_type, original.message_type);
    EXPECT_EQ(restored.error_code, original.error_code);
    EXPECT_EQ(restored.interface_id, original.interface_id);
    EXPECT_EQ(restored.object_id, original.object_id);
    EXPECT_EQ(restored.version, original.version);
    EXPECT_EQ(restored.flags, original.flags);
    EXPECT_EQ(restored.body_size, original.body_size);
}

// ====== Concurrency Tests ======

TEST_F(IpcMessageQueueTransportTest, Send_MultipleMessagesSequential)
{
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 100),
        DAS_S_OK);

    for (int i = 0; i < 10; ++i)
    {
        auto header = CreateTestHeader();
        header.call_id = static_cast<uint64_t>(i);
        uint8_t body[] = {static_cast<uint8_t>(i)};

        auto result = transport_->Send(header, body, sizeof(body));
        EXPECT_EQ(result, DAS_S_OK);
    }
}

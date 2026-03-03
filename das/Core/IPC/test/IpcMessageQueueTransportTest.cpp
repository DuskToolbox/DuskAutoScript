#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcTransport.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::IPCMessageHeaderBuilder;
using DAS::Core::IPC::IpcTransport;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::ValidatedIPCMessageHeader;

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

    ValidatedIPCMessageHeader CreateTestHeader(
        MessageType type = MessageType::REQUEST,
        uint64_t    call_id = 1,
        uint32_t    body_size = 0)
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(type)
            .SetCallId(call_id)
            .SetBusinessInterface(1, 0)
            .SetFlags(0)
            .SetBodySize(body_size)
            .Build();
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

    uint8_t      body[] = {1, 2, 3, 4, 5};
    const size_t body_size = sizeof(body);

    auto header = CreateTestHeader(
        MessageType::REQUEST,
        1,
        static_cast<uint32_t>(body_size));
    auto result = transport_->Send(header, body, body_size);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcMessageQueueTransportTest, Send_SmallMessageWithNullBody)
{
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10),
        DAS_S_OK);

    auto header = CreateTestHeader(MessageType::REQUEST, 1, 0);
    auto result = transport_->Send(header, nullptr, 0);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcMessageQueueTransportTest, Receive_SmallMessage)
{
    // Server creates queues
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10),
        DAS_S_OK);

    // Client connects to server's queues (swap direction)
    auto client = std::make_unique<IpcTransport>();
    ASSERT_EQ(client->Connect(host_queue_name_, plugin_queue_name_), DAS_S_OK);

    uint8_t      body[] = {1, 2, 3, 4, 5};
    const size_t body_size = sizeof(body);

    auto header = CreateTestHeader(
        MessageType::REQUEST,
        1,
        static_cast<uint32_t>(body_size));

    // Server sends, client receives
    ASSERT_EQ(transport_->Send(header, body, body_size), DAS_S_OK);

    IPCMessageHeader     recv_header;
    std::vector<uint8_t> recv_body;
    auto                 result = client->Receive(recv_header, recv_body, 1000);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(recv_header.call_id, header.Raw().call_id);
    EXPECT_EQ(recv_body.size(), body_size);

    client->Shutdown();
}

// ====== Large Message Tests (> 4KB) ======

TEST_F(IpcMessageQueueTransportTest, Send_LargeMessageRequiresSharedMemory)
{
    // Large message requires shared memory pool
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 10),
        DAS_S_OK);

    std::vector<uint8_t> large_body(8192, 0xAB); // 8KB
    auto                 header = CreateTestHeader(
        MessageType::REQUEST,
        1,
        static_cast<uint32_t>(large_body.size()));

    // Without shared memory pool, should fail
    auto result =
        transport_->Send(header, large_body.data(), large_body.size());
    EXPECT_NE(result, DAS_S_OK); // Expected to fail without SHM pool
}

// ====== MakeQueueName Tests ======

TEST_F(IpcMessageQueueTransportTest, MakeQueueName_HostToPlugin)
{
    auto name = IpcTransport::MakeQueueName(1, 2, true);
    EXPECT_EQ(name, "das_ipc_1_2_m2h");
}

TEST_F(IpcMessageQueueTransportTest, MakeQueueName_PluginToHost)
{
    auto name = IpcTransport::MakeQueueName(1, 2, false);
    EXPECT_EQ(name, "das_ipc_1_2_h2m");
}

// ====== Error Cases ======

TEST_F(IpcMessageQueueTransportTest, Send_WithoutInitialize)
{
    uint8_t body[] = {1, 2, 3};
    auto    header = CreateTestHeader(MessageType::REQUEST, 1, sizeof(body));

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

// ====== Message Header V2 Tests ======

TEST_F(IpcMessageQueueTransportTest, HeaderV2_FieldsCorrect)
{
    auto header = IPCMessageHeaderBuilder()
                      .SetMessageType(MessageType::REQUEST)
                      .SetCallId(12345)
                      .SetBusinessInterface(999, 42)
                      .SetObject(1, 2, 0xDEAD)
                      .SetFlags(0xFF)
                      .SetBodySize(1024)
                      .Build();

    const auto& raw = header.Raw();
    EXPECT_EQ(raw.magic, IPCMessageHeader::MAGIC);
    EXPECT_EQ(raw.version, IPCMessageHeader::CURRENT_VERSION);
    EXPECT_EQ(raw.call_id, 12345ULL);
    EXPECT_EQ(raw.interface_id, 999U);
    EXPECT_EQ(raw.method_id, 42U);
    EXPECT_EQ(raw.session_id, 1U);
    EXPECT_EQ(raw.generation, 2U);
    EXPECT_EQ(raw.local_id, 0xDEADU);
    EXPECT_EQ(raw.flags, 0xFFU);
    EXPECT_EQ(raw.body_size, 1024U);
}

// ====== Concurrency Tests ======

TEST_F(IpcMessageQueueTransportTest, Send_MultipleMessagesSequential)
{
    ASSERT_EQ(
        transport_->Initialize(host_queue_name_, plugin_queue_name_, 4096, 100),
        DAS_S_OK);

    for (int i = 0; i < 10; ++i)
    {
        uint8_t body[] = {static_cast<uint8_t>(i)};
        auto    header = CreateTestHeader(
            MessageType::REQUEST,
            static_cast<uint64_t>(i),
            sizeof(body));

        auto result = transport_->Send(header, body, sizeof(body));
        EXPECT_EQ(result, DAS_S_OK);
    }
}

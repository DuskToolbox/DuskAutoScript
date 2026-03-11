#include <chrono>
#include <cstring>
#include <das/Core/IPC/Host/HandshakeHandler.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <gtest/gtest.h>
#include <thread>

using namespace DAS::Core::IPC;
using namespace DAS::Core::IPC::Host;

// Test fixture for HandshakeHandler tests
class HandshakeHandlerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 使用 RAII 工厂函数创建（session_id = 0 表示等待握手分配）
        handler_ = HandshakeHandler::Create(0);
        ASSERT_NE(handler_, nullptr);
    }

    void TearDown() override
    {
        // 析构函数自动调用 Uninitialize()
        handler_.reset();
    }

    // Helper to create a HelloRequest and simulate client registration
    void RegisterMockClient(uint16_t session_id, uint32_t pid, const char* plugin_name)
    {
        HelloRequestV1 request = {};
        request.protocol_version = HelloRequestV1::CURRENT_PROTOCOL_VERSION;
        request.pid = pid;
        request.assigned_session_id = session_id;
        std::strncpy(request.plugin_name, plugin_name, sizeof(request.plugin_name) - 1);

        auto validated_header = IPCMessageHeaderBuilder()
                          .SetMessageType(MessageType::REQUEST)
                          .SetBusinessInterface(
                              static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO),
                              0)
                          .SetBodySize(sizeof(request))
                          .SetCallId(next_call_id_++)
                          .Build();

        std::vector<uint8_t> response_body;
        handler_->HandleMessage(
            validated_header.Raw(),
            reinterpret_cast<const uint8_t*>(&request),
            sizeof(request),
            response_body);
    }

    // Helper to send heartbeat using the old interface
    DasResult SendHeartbeat(uint16_t sender_session_id)
    {
        HeartbeatV1 heartbeat = {};
        heartbeat.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

        auto validated_header = IPCMessageHeaderBuilder()
                          .SetMessageType(MessageType::REQUEST)
                          .SetBusinessInterface(
                              static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_HEARTBEAT),
                              0)
                          .SetBodySize(sizeof(heartbeat))
                          .SetCallId(next_call_id_++)
                          .SetSessionId(sender_session_id)
                          .Build();

        std::vector<uint8_t> response_body;
        return handler_->HandleMessage(
            validated_header.Raw(),
            reinterpret_cast<const uint8_t*>(&heartbeat),
            sizeof(heartbeat),
            response_body);
    }

    std::unique_ptr<HandshakeHandler> handler_;
    uint32_t                            next_call_id_ = 1;
};

// ====== HandleHeartbeat Tests ======

TEST_F(HandshakeHandlerTest, HandleHeartbeat_UpdatesOnlySenderClient)
{
    // 1. 注册两个客户端
    RegisterMockClient(2, 1001, "PluginA");
    RegisterMockClient(3, 1002, "PluginB");

    ASSERT_EQ(handler_->GetClientCount(), 2u);

    // 2. 获取两个客户端的初始心跳时间
    const ConnectedClient* client1 = handler_->GetClient(2);
    const ConnectedClient* client2 = handler_->GetClient(3);
    ASSERT_NE(client1, nullptr);
    ASSERT_NE(client2, nullptr);

    auto initial_heartbeat_1 = client1->last_heartbeat;
    auto initial_heartbeat_2 = client2->last_heartbeat;

    // 等待一小段时间确保时间戳差异
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 3. 发送心跳，指定 session_id = 2
    auto result = SendHeartbeat(2);
    EXPECT_EQ(result, DAS_S_OK);

    // 4. 验证只有 session_id = 2 的客户端的心跳时间被更新
    client1 = handler_->GetClient(2);
    client2 = handler_->GetClient(3);
    ASSERT_NE(client1, nullptr);
    ASSERT_NE(client2, nullptr);

    // client1 的时间应该已更新
    EXPECT_GT(client1->last_heartbeat, initial_heartbeat_1);
    // client2 的时间应该保持不变
    EXPECT_EQ(client2->last_heartbeat, initial_heartbeat_2);
}

TEST_F(HandshakeHandlerTest, HandleHeartbeat_NonExistentClientNoCrash)
{
    // 测试当心跳来自不存在的客户端时，系统不崩溃
    auto result = SendHeartbeat(9999);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(HandshakeHandlerTest, HandleHeartbeat_MultipleClientsIndependent)
{
    // 注册三个客户端
    RegisterMockClient(10, 2001, "Plugin1");
    RegisterMockClient(20, 2002, "Plugin2");
    RegisterMockClient(30, 2003, "Plugin3");

    ASSERT_EQ(handler_->GetClientCount(), 3u);

    // 记录初始心跳时间
    auto initial_10 = handler_->GetClient(10)->last_heartbeat;
    auto initial_20 = handler_->GetClient(20)->last_heartbeat;
    auto initial_30 = handler_->GetClient(30)->last_heartbeat;

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 发送心跳给 session_id = 20
    SendHeartbeat(20);

    // 验证只有 session_id = 20 的客户端被更新
    EXPECT_EQ(handler_->GetClient(10)->last_heartbeat, initial_10);
    EXPECT_GT(handler_->GetClient(20)->last_heartbeat, initial_20);
    EXPECT_EQ(handler_->GetClient(30)->last_heartbeat, initial_30);
}

// ====== Basic Tests ======

TEST_F(HandshakeHandlerTest, Initialize_Succeeds)
{
    EXPECT_TRUE(handler_->IsInitialized());
}

TEST_F(HandshakeHandlerTest, GetClientCount_InitiallyZero)
{
    EXPECT_EQ(handler_->GetClientCount(), 0u);
}

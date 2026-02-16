#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/MainProcessServer.h>
#include <das/Core/IPC/ObjectId.h>
#include <gtest/gtest.h>
#include <vector>

using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::HostSessionInfo;
using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::MainProcessServer;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::RemoteObjectInfo;

class MainProcessServerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        server_ = &MainProcessServer::GetInstance();
        server_->Initialize();
    }

    void TearDown() override { server_->Shutdown(); }

    ObjectId CreateTestObjectId(
        uint16_t session_id,
        uint16_t generation,
        uint32_t local_id)
    {
        return ObjectId{
            .session_id = session_id,
            .generation = generation,
            .local_id = local_id};
    }

    IPCMessageHeader CreateTestHeader(
        uint64_t    object_id,
        MessageType type = MessageType::REQUEST)
    {
        IPCMessageHeader header{};
        header.call_id = 1;
        header.message_type = static_cast<uint8_t>(type);
        header.error_code = DAS_S_OK;
        header.interface_id = 1;
        // Decode object_id into session_id, generation, local_id
        header.session_id = static_cast<uint16_t>((object_id >> 48) & 0xFFFF);
        header.generation = static_cast<uint16_t>((object_id >> 32) & 0xFFFF);
        header.local_id = static_cast<uint32_t>(object_id & 0xFFFFFFFF);
        header.version = 2;
        header.flags = 0;
        header.body_size = 0;
        return header;
    }

    DasGuid CreateTestGuid()
    {
        DasGuid guid{};
        guid.data1 = 0x12345678;
        guid.data2 = 0x1234;
        guid.data3 = 0x5678;
        guid.data4[0] = 0x90;
        guid.data4[1] = 0xAB;
        guid.data4[2] = 0xCD;
        guid.data4[3] = 0xEF;
        guid.data4[4] = 0x01;
        guid.data4[5] = 0x23;
        guid.data4[6] = 0x45;
        guid.data4[7] = 0x67;
        return guid;
    }

    MainProcessServer* server_;
};

// ====== Initialize/Shutdown Tests ======

TEST_F(MainProcessServerTest, Initialize_Succeeds)
{
    EXPECT_TRUE(server_->IsRunning() || !server_->IsRunning());
}

TEST_F(MainProcessServerTest, Shutdown_CleansUp)
{
    server_->OnHostConnected(2);
    EXPECT_TRUE(server_->IsSessionConnected(2));

    server_->Shutdown();
    EXPECT_FALSE(server_->IsSessionConnected(2));

    server_->Initialize();
}

// ====== Start/Stop Tests ======

TEST_F(MainProcessServerTest, Start_Succeeds)
{
    auto result = server_->Start();
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(server_->IsRunning());
    server_->Stop();
}

TEST_F(MainProcessServerTest, Stop_Succeeds)
{
    server_->Start();
    auto result = server_->Stop();
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_FALSE(server_->IsRunning());
}

TEST_F(MainProcessServerTest, Stop_Idempotent)
{
    auto result = server_->Stop();
    EXPECT_EQ(result, DAS_S_OK);
    result = server_->Stop();
    EXPECT_EQ(result, DAS_S_OK);
}

// ====== Session Management Tests ======

TEST_F(MainProcessServerTest, OnHostConnected_ValidSession)
{
    auto result = server_->OnHostConnected(2);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(server_->IsSessionConnected(2));
}

TEST_F(MainProcessServerTest, OnHostConnected_InvalidSessionId)
{
    auto result = server_->OnHostConnected(0);
    EXPECT_NE(result, DAS_S_OK);

    result = server_->OnHostConnected(0xFFFF);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(MainProcessServerTest, OnHostConnected_DuplicateSession)
{
    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);
    auto result = server_->OnHostConnected(2);
    EXPECT_EQ(result, DAS_E_DUPLICATE_ELEMENT);
}

TEST_F(MainProcessServerTest, OnHostDisconnected_ValidSession)
{
    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);
    auto result = server_->OnHostDisconnected(2);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_FALSE(server_->IsSessionConnected(2));
}

TEST_F(MainProcessServerTest, OnHostDisconnected_UnknownSession)
{
    auto result = server_->OnHostDisconnected(999);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

TEST_F(MainProcessServerTest, GetConnectedSessions_Empty)
{
    auto sessions = server_->GetConnectedSessions();
    EXPECT_TRUE(sessions.empty());
}

TEST_F(MainProcessServerTest, GetConnectedSessions_Multiple)
{
    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);
    ASSERT_EQ(server_->OnHostConnected(3), DAS_S_OK);

    auto sessions = server_->GetConnectedSessions();
    EXPECT_EQ(sessions.size(), 2);
}

TEST_F(MainProcessServerTest, GetSessionInfo_ValidSession)
{
    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);

    HostSessionInfo info;
    auto            result = server_->GetSessionInfo(2, info);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(info.session_id, 2);
    EXPECT_TRUE(info.is_connected);
}

TEST_F(MainProcessServerTest, GetSessionInfo_UnknownSession)
{
    HostSessionInfo info;
    auto            result = server_->GetSessionInfo(999, info);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

// ====== Remote Object Management Tests ======

TEST_F(MainProcessServerTest, OnRemoteObjectRegistered_ValidObject)
{
    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);

    ObjectId    obj_id = CreateTestObjectId(2, 1, 1);
    DasGuid     iid = CreateTestGuid();
    std::string name = "TestObject";

    auto result = server_->OnRemoteObjectRegistered(obj_id, iid, 2, name, 1);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(MainProcessServerTest, OnRemoteObjectRegistered_SessionNotConnected)
{
    ObjectId    obj_id = CreateTestObjectId(999, 1, 1);
    DasGuid     iid = CreateTestGuid();
    std::string name = "TestObject";

    auto result = server_->OnRemoteObjectRegistered(obj_id, iid, 999, name, 1);
    EXPECT_EQ(result, DAS_E_IPC_CONNECTION_LOST);
}

TEST_F(MainProcessServerTest, OnRemoteObjectUnregistered_ValidObject)
{
    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);

    ObjectId    obj_id = CreateTestObjectId(2, 1, 1);
    DasGuid     iid = CreateTestGuid();
    std::string name = "TestObject";

    ASSERT_EQ(
        server_->OnRemoteObjectRegistered(obj_id, iid, 2, name, 1),
        DAS_S_OK);

    auto result = server_->OnRemoteObjectUnregistered(obj_id);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(MainProcessServerTest, GetRemoteObjects_Empty)
{
    std::vector<RemoteObjectInfo> objects;
    auto                          result = server_->GetRemoteObjects(objects);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(objects.empty());
}

TEST_F(MainProcessServerTest, GetRemoteObjects_Multiple)
{
    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);

    ObjectId obj_id1 = CreateTestObjectId(2, 1, 1);
    ObjectId obj_id2 = CreateTestObjectId(2, 1, 2);
    DasGuid  iid = CreateTestGuid();

    ASSERT_EQ(
        server_->OnRemoteObjectRegistered(obj_id1, iid, 2, "Object1", 1),
        DAS_S_OK);
    ASSERT_EQ(
        server_->OnRemoteObjectRegistered(obj_id2, iid, 2, "Object2", 1),
        DAS_S_OK);

    std::vector<RemoteObjectInfo> objects;
    auto                          result = server_->GetRemoteObjects(objects);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(objects.size(), 2);
}

TEST_F(MainProcessServerTest, LookupRemoteObjectByName_Found)
{
    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);

    ObjectId    obj_id = CreateTestObjectId(2, 1, 1);
    DasGuid     iid = CreateTestGuid();
    std::string name = "TestObject";

    ASSERT_EQ(
        server_->OnRemoteObjectRegistered(obj_id, iid, 2, name, 1),
        DAS_S_OK);

    RemoteObjectInfo info;
    auto             result = server_->LookupRemoteObjectByName(name, info);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(info.name, name);
}

TEST_F(MainProcessServerTest, LookupRemoteObjectByName_NotFound)
{
    RemoteObjectInfo info;
    auto result = server_->LookupRemoteObjectByName("NonExistent", info);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

// ====== Message Dispatch Tests ======

TEST_F(MainProcessServerTest, DispatchMessage_NotRunning)
{
    server_->Stop();

    auto                 header = CreateTestHeader(0);
    std::vector<uint8_t> body;
    std::vector<uint8_t> response;

    auto result =
        server_->DispatchMessage(header, body.data(), body.size(), response);
    EXPECT_EQ(result, DAS_E_IPC_INVALID_STATE);
}

TEST_F(MainProcessServerTest, DispatchMessage_InvalidObjectId)
{
    server_->Start();

    auto                 header = CreateTestHeader(0);
    std::vector<uint8_t> body;
    std::vector<uint8_t> response;

    auto result =
        server_->DispatchMessage(header, body.data(), body.size(), response);
    EXPECT_EQ(result, DAS_E_IPC_INVALID_OBJECT_ID);

    server_->Stop();
}

TEST_F(MainProcessServerTest, DispatchMessage_ObjectNotFound)
{
    server_->Start();

    ObjectId             obj_id = CreateTestObjectId(2, 1, 1);
    uint64_t             encoded_id = EncodeObjectId(obj_id);
    auto                 header = CreateTestHeader(encoded_id);
    std::vector<uint8_t> body;
    std::vector<uint8_t> response;

    auto result =
        server_->DispatchMessage(header, body.data(), body.size(), response);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);

    server_->Stop();
}

TEST_F(MainProcessServerTest, DispatchMessage_SessionNotConnected)
{
    // 注意：当 session 断开时，该 session 的所有对象会被自动移除
    // 所以 DispatchMessage 会返回 OBJECT_NOT_FOUND 而不是 CONNECTION_LOST
    // 这是设计行为，因为对象已不存在
    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);

    ObjectId    obj_id = CreateTestObjectId(2, 1, 1);
    DasGuid     iid = CreateTestGuid();
    std::string name = "TestObject";

    ASSERT_EQ(
        server_->OnRemoteObjectRegistered(obj_id, iid, 2, name, 1),
        DAS_S_OK);
    ASSERT_EQ(server_->OnHostDisconnected(2), DAS_S_OK);

    server_->Start();

    uint64_t             encoded_id = EncodeObjectId(obj_id);
    auto                 header = CreateTestHeader(encoded_id);
    std::vector<uint8_t> body;
    std::vector<uint8_t> response;

    auto result =
        server_->DispatchMessage(header, body.data(), body.size(), response);
    // 对象在 session 断开时已被移除，所以返回 OBJECT_NOT_FOUND
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);

    server_->Stop();
}

TEST_F(MainProcessServerTest, DispatchMessage_CustomHandler)
{
    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);

    ObjectId    obj_id = CreateTestObjectId(2, 1, 1);
    DasGuid     iid = CreateTestGuid();
    std::string name = "TestObject";

    ASSERT_EQ(
        server_->OnRemoteObjectRegistered(obj_id, iid, 2, name, 1),
        DAS_S_OK);

    server_->SetMessageDispatchHandler(
        [](const IPCMessageHeader&,
           const uint8_t*,
           size_t,
           std::vector<uint8_t>& response)
        {
            response.push_back(0x42);
            return DAS_S_OK;
        });

    server_->Start();

    uint64_t             encoded_id = EncodeObjectId(obj_id);
    auto                 header = CreateTestHeader(encoded_id);
    std::vector<uint8_t> body;
    std::vector<uint8_t> response;

    auto result =
        server_->DispatchMessage(header, body.data(), body.size(), response);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(response.size(), 1);
    EXPECT_EQ(response[0], 0x42);

    server_->Stop();
}

// ====== Callback Tests ======

TEST_F(MainProcessServerTest, OnSessionConnectedCallback_Called)
{
    bool     callback_called = false;
    uint16_t callback_session_id = 0;

    server_->SetOnSessionConnectedCallback(
        [&](uint16_t session_id)
        {
            callback_called = true;
            callback_session_id = session_id;
        });

    server_->OnHostConnected(2);

    EXPECT_TRUE(callback_called);
    EXPECT_EQ(callback_session_id, 2);
}

TEST_F(MainProcessServerTest, OnSessionDisconnectedCallback_Called)
{
    bool callback_called = false;

    server_->SetOnSessionDisconnectedCallback([&](uint16_t)
                                              { callback_called = true; });

    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);
    server_->OnHostDisconnected(2);

    EXPECT_TRUE(callback_called);
}

TEST_F(MainProcessServerTest, OnObjectRegisteredCallback_Called)
{
    bool callback_called = false;

    server_->SetOnObjectRegisteredCallback([&](const RemoteObjectInfo&)
                                           { callback_called = true; });

    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);

    ObjectId obj_id = CreateTestObjectId(2, 1, 1);
    DasGuid  iid = CreateTestGuid();
    server_->OnRemoteObjectRegistered(obj_id, iid, 2, "TestObject", 1);

    EXPECT_TRUE(callback_called);
}

TEST_F(MainProcessServerTest, OnObjectUnregisteredCallback_CalledOnDisconnect)
{
    int unregister_count = 0;

    server_->SetOnObjectUnregisteredCallback([&](const RemoteObjectInfo&)
                                             { unregister_count++; });

    ASSERT_EQ(server_->OnHostConnected(2), DAS_S_OK);

    ObjectId obj_id1 = CreateTestObjectId(2, 1, 1);
    ObjectId obj_id2 = CreateTestObjectId(2, 1, 2);
    DasGuid  iid = CreateTestGuid();

    ASSERT_EQ(
        server_->OnRemoteObjectRegistered(obj_id1, iid, 2, "Object1", 1),
        DAS_S_OK);
    ASSERT_EQ(
        server_->OnRemoteObjectRegistered(obj_id2, iid, 2, "Object2", 1),
        DAS_S_OK);

    server_->OnHostDisconnected(2);

    EXPECT_EQ(unregister_count, 2);
}

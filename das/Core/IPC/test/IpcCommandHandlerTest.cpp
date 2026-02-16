#include <cstdint>
#include <cstring>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <gtest/gtest.h>
#include <vector>

using DAS::Core::IPC::ClearSessionPayload;
using DAS::Core::IPC::IpcCommandHandler;
using DAS::Core::IPC::IpcCommandResponse;
using DAS::Core::IPC::IpcCommandType;
using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::ListSessionObjectsPayload;
using DAS::Core::IPC::LookupByInterfacePayload;
using DAS::Core::IPC::LookupByNamePayload;
using DAS::Core::IPC::LookupObjectPayload;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::RemoteObjectRegistry;
using DAS::Core::IPC::UnregisterObjectPayload;

namespace
{
    DasGuid CreateTestGuid(
        uint32_t data1,
        uint16_t data2,
        uint16_t data3,
        uint8_t  b1,
        uint8_t  b2,
        uint8_t  b3,
        uint8_t  b4,
        uint8_t  b5,
        uint8_t  b6,
        uint8_t  b7,
        uint8_t  b8)
    {
        DasGuid guid;
        guid.data1 = data1;
        guid.data2 = data2;
        guid.data3 = data3;
        guid.data4[0] = b1;
        guid.data4[1] = b2;
        guid.data4[2] = b3;
        guid.data4[3] = b4;
        guid.data4[4] = b5;
        guid.data4[5] = b6;
        guid.data4[6] = b7;
        guid.data4[7] = b8;
        return guid;
    }
}

template <typename T>
void AppendToBuffer(std::vector<uint8_t>& buffer, const T& value)
{
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
}

void AppendString(std::vector<uint8_t>& buffer, const std::string& str)
{
    AppendToBuffer(buffer, static_cast<uint16_t>(str.size()));
    buffer.insert(
        buffer.end(),
        reinterpret_cast<const uint8_t*>(str.data()),
        reinterpret_cast<const uint8_t*>(str.data()) + str.size());
}

template <typename T>
T ReadFromBuffer(const std::vector<uint8_t>& buffer, size_t& offset)
{
    T value;
    std::memcpy(&value, buffer.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

std::string ReadStringFromBuffer(
    const std::vector<uint8_t>& buffer,
    size_t&                     offset)
{
    uint16_t    len = ReadFromBuffer<uint16_t>(buffer, offset);
    std::string str(reinterpret_cast<const char*>(buffer.data() + offset), len);
    offset += len;
    return str;
}

IPCMessageHeader MakeHeader(IpcCommandType cmd_type)
{
    IPCMessageHeader header{};
    header.call_id = 1;
    header.message_type = static_cast<uint8_t>(MessageType::REQUEST);
    header.error_code = 0;
    header.interface_id = static_cast<uint32_t>(cmd_type);
    header.session_id = 0;
    header.generation = 0;
    header.local_id = 0;
    header.version = 1;
    header.flags = 0;
    header.body_size = 0;
    return header;
}

class IpcCommandHandlerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        RemoteObjectRegistry::GetInstance().Clear();
        handler_.SetSessionId(1);
    }

    void TearDown() override { RemoteObjectRegistry::GetInstance().Clear(); }

    IpcCommandHandler handler_;
};

TEST_F(IpcCommandHandlerTest, RegisterObject_Success)
{
    std::vector<uint8_t> payload;
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    AppendToBuffer(payload, obj_id);
    AppendToBuffer(payload, iid);
    AppendToBuffer(payload, static_cast<uint16_t>(1));
    AppendToBuffer(payload, static_cast<uint16_t>(1));
    AppendString(payload, "test_object");

    auto               header = MakeHeader(IpcCommandType::REGISTER_OBJECT);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(response.error_code, DAS_S_OK);
    EXPECT_TRUE(RemoteObjectRegistry::GetInstance().ObjectExists(obj_id));
}

TEST_F(IpcCommandHandlerTest, RegisterObject_InvalidObjectId)
{
    std::vector<uint8_t> payload;
    ObjectId obj_id{.session_id = 0, .generation = 0, .local_id = 0};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    AppendToBuffer(payload, obj_id);
    AppendToBuffer(payload, iid);
    AppendToBuffer(payload, static_cast<uint16_t>(1));
    AppendToBuffer(payload, static_cast<uint16_t>(1));
    AppendString(payload, "test_object");

    auto               header = MakeHeader(IpcCommandType::REGISTER_OBJECT);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_IPC_INVALID_OBJECT_ID);
}

TEST_F(IpcCommandHandlerTest, RegisterObject_Duplicate)
{
    std::vector<uint8_t> payload;
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    AppendToBuffer(payload, obj_id);
    AppendToBuffer(payload, iid);
    AppendToBuffer(payload, static_cast<uint16_t>(1));
    AppendToBuffer(payload, static_cast<uint16_t>(1));
    AppendString(payload, "test_object");

    auto               header = MakeHeader(IpcCommandType::REGISTER_OBJECT);
    IpcCommandResponse response;

    handler_.HandleCommand(header, payload, response);

    payload.clear();
    AppendToBuffer(payload, obj_id);
    AppendToBuffer(payload, iid);
    AppendToBuffer(payload, static_cast<uint16_t>(1));
    AppendToBuffer(payload, static_cast<uint16_t>(1));
    AppendString(payload, "test_object");

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_DUPLICATE_ELEMENT);
}

TEST_F(IpcCommandHandlerTest, UnregisterObject_Success)
{
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    RemoteObjectRegistry::GetInstance()
        .RegisterObject(obj_id, iid, 1, "test_object", 1);

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, obj_id);

    auto               header = MakeHeader(IpcCommandType::UNREGISTER_OBJECT);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_FALSE(RemoteObjectRegistry::GetInstance().ObjectExists(obj_id));
}

TEST_F(IpcCommandHandlerTest, UnregisterObject_NotFound)
{
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 999};

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, obj_id);

    auto               header = MakeHeader(IpcCommandType::UNREGISTER_OBJECT);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

TEST_F(IpcCommandHandlerTest, LookupObject_Success)
{
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    RemoteObjectRegistry::GetInstance()
        .RegisterObject(obj_id, iid, 1, "test_object", 2);

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, obj_id);

    auto               header = MakeHeader(IpcCommandType::LOOKUP_OBJECT);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t   offset = 0;
    ObjectId returned_id =
        ReadFromBuffer<ObjectId>(response.response_data, offset);
    EXPECT_EQ(returned_id.session_id, 1);
    EXPECT_EQ(returned_id.local_id, 100);

    DasGuid returned_iid =
        ReadFromBuffer<DasGuid>(response.response_data, offset);
    EXPECT_EQ(returned_iid.data1, 0x12345678);

    uint16_t returned_session =
        ReadFromBuffer<uint16_t>(response.response_data, offset);
    EXPECT_EQ(returned_session, 1);

    uint16_t returned_version =
        ReadFromBuffer<uint16_t>(response.response_data, offset);
    EXPECT_EQ(returned_version, 2);

    std::string name = ReadStringFromBuffer(response.response_data, offset);
    EXPECT_EQ(name, "test_object");
}

TEST_F(IpcCommandHandlerTest, LookupObject_NotFound)
{
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 999};

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, obj_id);

    auto               header = MakeHeader(IpcCommandType::LOOKUP_OBJECT);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

TEST_F(IpcCommandHandlerTest, LookupByName_Success)
{
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    RemoteObjectRegistry::GetInstance()
        .RegisterObject(obj_id, iid, 1, "test_object", 1);

    std::vector<uint8_t> payload;
    AppendString(payload, "test_object");

    auto               header = MakeHeader(IpcCommandType::LOOKUP_BY_NAME);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t   offset = 0;
    ObjectId returned_id =
        ReadFromBuffer<ObjectId>(response.response_data, offset);
    EXPECT_EQ(returned_id.local_id, 100);
}

TEST_F(IpcCommandHandlerTest, LookupByName_NotFound)
{
    std::vector<uint8_t> payload;
    AppendString(payload, "nonexistent");

    auto               header = MakeHeader(IpcCommandType::LOOKUP_BY_NAME);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

TEST_F(IpcCommandHandlerTest, LookupByInterface_Success)
{
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    RemoteObjectRegistry::GetInstance()
        .RegisterObject(obj_id, iid, 1, "test_object", 1);

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, iid);

    auto               header = MakeHeader(IpcCommandType::LOOKUP_BY_INTERFACE);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t   offset = 0;
    ObjectId returned_id =
        ReadFromBuffer<ObjectId>(response.response_data, offset);
    EXPECT_EQ(returned_id.local_id, 100);
}

TEST_F(IpcCommandHandlerTest, LookupByInterface_NotFound)
{
    DasGuid iid = CreateTestGuid(
        0x87654321,
        0x4321,
        0x8765,
        0x21,
        0x43,
        0x65,
        0x87,
        0x65,
        0x43,
        0x21,
        0xF0);

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, iid);

    auto               header = MakeHeader(IpcCommandType::LOOKUP_BY_INTERFACE);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

TEST_F(IpcCommandHandlerTest, ListObjects_Empty)
{
    std::vector<uint8_t> payload;

    auto               header = MakeHeader(IpcCommandType::LIST_OBJECTS);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t   offset = 0;
    uint32_t count = ReadFromBuffer<uint32_t>(response.response_data, offset);
    EXPECT_EQ(count, 0);
}

TEST_F(IpcCommandHandlerTest, ListObjects_WithObjects)
{
    ObjectId obj1{.session_id = 1, .generation = 1, .local_id = 100};
    ObjectId obj2{.session_id = 2, .generation = 1, .local_id = 200};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    RemoteObjectRegistry::GetInstance().RegisterObject(obj1, iid, 1, "obj1", 1);
    RemoteObjectRegistry::GetInstance().RegisterObject(obj2, iid, 2, "obj2", 1);

    std::vector<uint8_t> payload;

    auto               header = MakeHeader(IpcCommandType::LIST_OBJECTS);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t   offset = 0;
    uint32_t count = ReadFromBuffer<uint32_t>(response.response_data, offset);
    EXPECT_EQ(count, 2);
}

TEST_F(IpcCommandHandlerTest, ListSessionObjects_Success)
{
    ObjectId obj1{.session_id = 1, .generation = 1, .local_id = 100};
    ObjectId obj2{.session_id = 2, .generation = 1, .local_id = 200};
    ObjectId obj3{.session_id = 1, .generation = 1, .local_id = 300};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    RemoteObjectRegistry::GetInstance().RegisterObject(obj1, iid, 1, "obj1", 1);
    RemoteObjectRegistry::GetInstance().RegisterObject(obj2, iid, 2, "obj2", 1);
    RemoteObjectRegistry::GetInstance().RegisterObject(obj3, iid, 1, "obj3", 1);

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, static_cast<uint16_t>(1));

    auto header = MakeHeader(IpcCommandType::LIST_SESSION_OBJECTS);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t   offset = 0;
    uint32_t count = ReadFromBuffer<uint32_t>(response.response_data, offset);
    EXPECT_EQ(count, 2);
}

TEST_F(IpcCommandHandlerTest, ClearSession_Success)
{
    ObjectId obj1{.session_id = 1, .generation = 1, .local_id = 100};
    ObjectId obj2{.session_id = 2, .generation = 1, .local_id = 200};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    RemoteObjectRegistry::GetInstance().RegisterObject(obj1, iid, 1, "obj1", 1);
    RemoteObjectRegistry::GetInstance().RegisterObject(obj2, iid, 2, "obj2", 1);

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, static_cast<uint16_t>(1));

    auto               header = MakeHeader(IpcCommandType::CLEAR_SESSION);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_FALSE(RemoteObjectRegistry::GetInstance().ObjectExists(obj1));
    EXPECT_TRUE(RemoteObjectRegistry::GetInstance().ObjectExists(obj2));
}

TEST_F(IpcCommandHandlerTest, Ping_Success)
{
    std::vector<uint8_t> payload;

    auto               header = MakeHeader(IpcCommandType::PING);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t   offset = 0;
    uint64_t timestamp =
        ReadFromBuffer<uint64_t>(response.response_data, offset);
    EXPECT_GT(timestamp, 0);
}

TEST_F(IpcCommandHandlerTest, GetObjectCount_Success)
{
    ObjectId obj1{.session_id = 1, .generation = 1, .local_id = 100};
    ObjectId obj2{.session_id = 2, .generation = 1, .local_id = 200};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    RemoteObjectRegistry::GetInstance().RegisterObject(obj1, iid, 1, "obj1", 1);
    RemoteObjectRegistry::GetInstance().RegisterObject(obj2, iid, 2, "obj2", 1);

    std::vector<uint8_t> payload;

    auto               header = MakeHeader(IpcCommandType::GET_OBJECT_COUNT);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t   offset = 0;
    uint64_t count = ReadFromBuffer<uint64_t>(response.response_data, offset);
    EXPECT_EQ(count, 2);
}

TEST_F(IpcCommandHandlerTest, InvalidCommand_ReturnsError)
{
    std::vector<uint8_t> payload;

    auto               header = MakeHeader(static_cast<IpcCommandType>(255));
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_IPC_INVALID_MESSAGE_TYPE);
}

TEST_F(IpcCommandHandlerTest, SetAndGetSessionId)
{
    handler_.SetSessionId(42);
    EXPECT_EQ(handler_.GetSessionId(), 42);

    handler_.SetSessionId(100);
    EXPECT_EQ(handler_.GetSessionId(), 100);
}

TEST_F(IpcCommandHandlerTest, RegisterObject_EmptyName)
{
    std::vector<uint8_t> payload;
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    AppendToBuffer(payload, obj_id);
    AppendToBuffer(payload, iid);
    AppendToBuffer(payload, static_cast<uint16_t>(1));
    AppendToBuffer(payload, static_cast<uint16_t>(1));
    AppendString(payload, "");

    auto               header = MakeHeader(IpcCommandType::REGISTER_OBJECT);
    IpcCommandResponse response;

    DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
}

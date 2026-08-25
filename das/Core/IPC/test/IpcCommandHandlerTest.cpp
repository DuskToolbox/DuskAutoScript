#include <boost/asio/use_future.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/RemoteRefHandler.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncCallback.h>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

#include "MockDasObject.h"

using DAS::Core::IPC::ClearSessionPayload;
using DAS::Core::IPC::ControlPlaneContext;
using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::IpcCommandHandler;
using DAS::Core::IPC::IpcCommandResponse;
using DAS::Core::IPC::IpcCommandType;
using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::IPCMessageHeaderBuilder;
using DAS::Core::IPC::IpcResponseSender;
using DAS::Core::IPC::IpcRunLoop;
using DAS::Core::IPC::ListSessionObjectsPayload;
using DAS::Core::IPC::LookupByInterfacePayload;
using DAS::Core::IPC::LookupByNamePayload;
using DAS::Core::IPC::LookupObjectPayload;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::ProxyFactory;
using DAS::Core::IPC::RemoteObjectRegistry;
using DAS::Core::IPC::RemoteRefHandler;
using DAS::Core::IPC::ValidatedIPCMessageHeader;
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
    const uint16_t len = ReadFromBuffer<uint16_t>(buffer, offset);
    std::string str(reinterpret_cast<const char*>(buffer.data() + offset), len);
    offset += len;
    return str;
}

ValidatedIPCMessageHeader MakeHeader(IpcCommandType cmd_type)
{
    IPCMessageHeader header{};
    header.call_id = 1;
    header.message_type = static_cast<uint8_t>(MessageType::Request);
    header.error_code = 0;
    header.interface_id = static_cast<uint32_t>(cmd_type);
    // V3: session_id/generation/local_id moved to body
    header.source_session_id = 0;
    header.target_session_id = 0;
    header.version = IPCMessageHeader::CURRENT_VERSION; // NOLINT:ipc-validator
    header.flags = 0;
    header.body_size = 0;
    return IPCMessageHeaderBuilder()
        .SetMessageType(MessageType::Request)
        .SetControlPlaneCommand(cmd_type)
        .SetCallId(1)
        .SetSourceSessionId(0)
        .SetTargetSessionId(0)
        .Build();
}

class IpcCommandHandlerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        registry_.Clear();
        object_manager_.SetSessionId(1);
        handler_.SetSessionId(1);
        run_loop_ = std::make_unique<IpcRunLoop>(
            false,
            &inbound_queue_,
            proxy_factory_,
            registry_);
        remote_ref_handler_ =
            RemoteRefHandler::Create(object_manager_, *run_loop_);
    }

    void TearDown() override
    {
        run_loop_.reset();
        registry_.Clear();
    }

    ProxyFactory             proxy_factory_;
    RemoteObjectRegistry     registry_;
    DistributedObjectManager object_manager_;
    IpcCommandHandler        handler_{registry_};
    DAS::Core::IPC::IpcMessageQueue<DAS::Core::IPC::InboundMessage>
                                  inbound_queue_{16};
    DAS::DasPtr<RemoteRefHandler> remote_ref_handler_;
    std::unique_ptr<IpcRunLoop>   run_loop_;
};

TEST_F(IpcCommandHandlerTest, LookupObject_Success)
{
    ObjectId      obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    const DasGuid iid = CreateTestGuid(
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

    registry_.RegisterObject(obj_id, iid, 1, "test_object", 2);

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, obj_id);

    auto               header = MakeHeader(IpcCommandType::LookupObject);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t   offset = 0;
    ObjectId returned_id =
        ReadFromBuffer<ObjectId>(response.response_data, offset);
    EXPECT_EQ(returned_id.session_id, 1);
    EXPECT_EQ(returned_id.local_id, 100);

    const DasGuid returned_iid =
        ReadFromBuffer<DasGuid>(response.response_data, offset);
    EXPECT_EQ(returned_iid.data1, 0x12345678);

    const uint16_t returned_session =
        ReadFromBuffer<uint16_t>(response.response_data, offset);
    EXPECT_EQ(returned_session, 1);

    const uint16_t returned_version =
        ReadFromBuffer<uint16_t>(response.response_data, offset);
    EXPECT_EQ(returned_version, 2);

    const std::string name =
        ReadStringFromBuffer(response.response_data, offset);
    EXPECT_EQ(name, "test_object");
}

TEST_F(IpcCommandHandlerTest, LookupObject_NotFound)
{
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 999};

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, obj_id);

    auto               header = MakeHeader(IpcCommandType::LookupObject);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

TEST_F(IpcCommandHandlerTest, LookupByName_Success)
{
    const DasGuid iid = CreateTestGuid(
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

    // resolve 的对象须先在 DOM 注册（与生产一致：服务经 RegisterLocalObject
    // 入表），OnLookupByName 才能在发出句柄时配对 DOM ref。
    MockDasObject* mock = new MockDasObject();
    ObjectId       obj_id{};
    ASSERT_EQ(object_manager_.RegisterLocalObject(mock, obj_id), DAS_S_OK);
    registry_.RegisterObject(obj_id, iid, 1, "test_object", 1);

    std::vector<uint8_t> payload;
    AppendString(payload, "test_object");

    auto               header = MakeHeader(IpcCommandType::LookupByName);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t   offset = 0;
    ObjectId returned_id =
        ReadFromBuffer<ObjectId>(response.response_data, offset);
    EXPECT_EQ(returned_id.session_id, obj_id.session_id);
    EXPECT_EQ(returned_id.local_id, obj_id.local_id);

    // Verify DasGuid in response matches the registered IID
    const DasGuid returned_iid =
        ReadFromBuffer<DasGuid>(response.response_data, offset);
    EXPECT_EQ(returned_iid.data1, 0x12345678);
    EXPECT_EQ(returned_iid.data2, 0x1234);
    EXPECT_EQ(returned_iid.data3, 0x5678);

    const uint16_t returned_session =
        ReadFromBuffer<uint16_t>(response.response_data, offset);
    EXPECT_EQ(returned_session, 1);

    const uint16_t returned_version =
        ReadFromBuffer<uint16_t>(response.response_data, offset);
    EXPECT_EQ(returned_version, 1);

    const std::string name =
        ReadStringFromBuffer(response.response_data, offset);
    EXPECT_EQ(name, "test_object");
}

TEST_F(IpcCommandHandlerTest, LookupByName_NotFound)
{
    std::vector<uint8_t> payload;
    AppendString(payload, "nonexistent");

    auto               header = MakeHeader(IpcCommandType::LookupByName);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

TEST_F(IpcCommandHandlerTest, LookupByInterface_Success)
{
    const DasGuid iid = CreateTestGuid(
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

    MockDasObject* mock = new MockDasObject();
    ObjectId       obj_id{};
    ASSERT_EQ(object_manager_.RegisterLocalObject(mock, obj_id), DAS_S_OK);
    registry_.RegisterObject(obj_id, iid, 1, "test_object", 1);

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, iid);

    auto               header = MakeHeader(IpcCommandType::LookupByInterface);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t   offset = 0;
    ObjectId returned_id =
        ReadFromBuffer<ObjectId>(response.response_data, offset);
    EXPECT_EQ(returned_id.local_id, obj_id.local_id);
}

TEST_F(IpcCommandHandlerTest, LookupByInterface_NotFound)
{
    const DasGuid iid = CreateTestGuid(
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

    auto               header = MakeHeader(IpcCommandType::LookupByInterface);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

// 同 session 重复 lookup（ByName / ByInterface）：零记账——对端物化配额由
// 物化申报持有，owner 不做本地 +1 猜测。观测锚点 = 条目回收：单次物化/
// 消亡 + 服务注销即归零，重复 lookup 产生的名额会让条目悬挂。
TEST_F(IpcCommandHandlerTest, RepeatedLookupsAddNoAccounting)
{
    const DasGuid iid = CreateTestGuid(
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

    MockDasObject* mock = new MockDasObject();
    ObjectId       obj_id{};
    ASSERT_EQ(object_manager_.RegisterLocalObject(mock, obj_id), DAS_S_OK);
    registry_.RegisterObject(obj_id, iid, 1, "test_object", 1);

    std::vector<uint8_t> by_name_payload;
    AppendString(by_name_payload, "test_object");
    std::vector<uint8_t> by_interface_payload;
    AppendToBuffer(by_interface_payload, iid);

    const auto by_name_header = MakeHeader(IpcCommandType::LookupByName);
    const auto by_interface_header =
        MakeHeader(IpcCommandType::LookupByInterface);

    const size_t entry_count_before = object_manager_.GetLocalEntryCount();

    for (int lookup_round = 0; lookup_round < 3; ++lookup_round)
    {
        IpcCommandResponse name_response;
        ASSERT_EQ(
            handler_
                .HandleCommand(by_name_header, by_name_payload, name_response),
            DAS_S_OK);
        IpcCommandResponse interface_response;
        ASSERT_EQ(
            handler_.HandleCommand(
                by_interface_header,
                by_interface_payload,
                interface_response),
            DAS_S_OK);
    }

    // 重复 lookup 零记账：条目数与远端 holder 数均不增长
    EXPECT_EQ(object_manager_.GetLocalEntryCount(), entry_count_before);
    EXPECT_EQ(object_manager_.GetRemoteHolderCount(obj_id), 0u);

    constexpr uint16_t peer_session_id = 5;
    ASSERT_EQ(
        object_manager_.AddRemoteHolder(obj_id, peer_session_id),
        DAS_S_OK);

    DAS::DasPtr<IDasBase> pin_released_object;
    ASSERT_EQ(
        object_manager_.UnregisterLocalPin(obj_id, pin_released_object),
        DAS_S_OK);

    DAS::DasPtr<IDasBase> holder_released_object;
    ASSERT_EQ(
        object_manager_.ReleaseRemoteHolder(
            obj_id,
            peer_session_id,
            holder_released_object),
        DAS_S_OK);

    // 单次物化/消亡 + 服务注销：条目回收
    EXPECT_EQ(object_manager_.GetLocalEntryCount(), 0u);
}

TEST_F(IpcCommandHandlerTest, ListObjects_Empty)
{
    std::vector<uint8_t> payload;

    auto               header = MakeHeader(IpcCommandType::ListObjects);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t         offset = 0;
    const uint32_t count =
        ReadFromBuffer<uint32_t>(response.response_data, offset);
    EXPECT_EQ(count, 0);
}

TEST_F(IpcCommandHandlerTest, ListObjects_WithObjects)
{
    ObjectId      obj1{.session_id = 1, .generation = 1, .local_id = 100};
    ObjectId      obj2{.session_id = 2, .generation = 1, .local_id = 200};
    const DasGuid iid = CreateTestGuid(
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

    registry_.RegisterObject(obj1, iid, 1, "obj1", 1);
    registry_.RegisterObject(obj2, iid, 2, "obj2", 1);

    std::vector<uint8_t> payload;

    auto               header = MakeHeader(IpcCommandType::ListObjects);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t         offset = 0;
    const uint32_t count =
        ReadFromBuffer<uint32_t>(response.response_data, offset);
    EXPECT_EQ(count, 2);
}

TEST_F(IpcCommandHandlerTest, ListSessionObjects_Success)
{
    ObjectId      obj1{.session_id = 1, .generation = 1, .local_id = 100};
    ObjectId      obj2{.session_id = 2, .generation = 1, .local_id = 200};
    ObjectId      obj3{.session_id = 1, .generation = 1, .local_id = 300};
    const DasGuid iid = CreateTestGuid(
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

    registry_.RegisterObject(obj1, iid, 1, "obj1", 1);
    registry_.RegisterObject(obj2, iid, 2, "obj2", 1);
    registry_.RegisterObject(obj3, iid, 1, "obj3", 1);

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, static_cast<uint16_t>(1));

    auto               header = MakeHeader(IpcCommandType::ListSessionObjects);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t         offset = 0;
    const uint32_t count =
        ReadFromBuffer<uint32_t>(response.response_data, offset);
    EXPECT_EQ(count, 2);
}

TEST_F(IpcCommandHandlerTest, ClearSession_Success)
{
    ObjectId      obj1{.session_id = 1, .generation = 1, .local_id = 100};
    ObjectId      obj2{.session_id = 2, .generation = 1, .local_id = 200};
    const DasGuid iid = CreateTestGuid(
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

    registry_.RegisterObject(obj1, iid, 1, "obj1", 1);
    registry_.RegisterObject(obj2, iid, 2, "obj2", 1);

    std::vector<uint8_t> payload;
    AppendToBuffer(payload, static_cast<uint16_t>(1));

    auto               header = MakeHeader(IpcCommandType::ClearSession);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_FALSE(registry_.ObjectExists(obj1));
    EXPECT_TRUE(registry_.ObjectExists(obj2));
}

TEST_F(IpcCommandHandlerTest, Ping_Success)
{
    std::vector<uint8_t> payload;

    auto               header = MakeHeader(IpcCommandType::Ping);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t         offset = 0;
    const uint64_t timestamp =
        ReadFromBuffer<uint64_t>(response.response_data, offset);
    EXPECT_GT(timestamp, 0);
}

TEST_F(IpcCommandHandlerTest, GetObjectCount_Success)
{
    ObjectId      obj1{.session_id = 1, .generation = 1, .local_id = 100};
    ObjectId      obj2{.session_id = 2, .generation = 1, .local_id = 200};
    const DasGuid iid = CreateTestGuid(
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

    registry_.RegisterObject(obj1, iid, 1, "obj1", 1);
    registry_.RegisterObject(obj2, iid, 2, "obj2", 1);

    std::vector<uint8_t> payload;

    auto               header = MakeHeader(IpcCommandType::GetObjectCount);
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_S_OK);

    size_t         offset = 0;
    const uint64_t count =
        ReadFromBuffer<uint64_t>(response.response_data, offset);
    EXPECT_EQ(count, 2);
}

TEST_F(IpcCommandHandlerTest, InvalidCommand_ReturnsError)
{
    std::vector<uint8_t> payload;

    auto               header = MakeHeader(static_cast<IpcCommandType>(255));
    IpcCommandResponse response;

    const DasResult result = handler_.HandleCommand(header, payload, response);

    EXPECT_EQ(result, DAS_E_IPC_INVALID_MESSAGE_TYPE);
}

TEST_F(IpcCommandHandlerTest, SetAndGetSessionId)
{
    handler_.SetSessionId(42);
    EXPECT_EQ(handler_.GetSessionId(), 42);

    handler_.SetSessionId(100);
    EXPECT_EQ(handler_.GetSessionId(), 100);
}

// ====== RemoteRefHandler（CONTROL_PLANE RemoteAddRef/RemoteRelease）======

namespace
{
    constexpr uint16_t kRemoteRefPeerSessionId = 5;

    /// 析构追踪对象：final Release 线程域/时序断言用（静态标记随对象
    /// 自毁存活）
    class DestructionTrackingObject final : public IDasBase
    {
    public:
        DestructionTrackingObject() { ResetTracking(); }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const uint32_t count = --ref_count_;
            if (count == 0)
            {
                destroyed_on_thread_.store(
                    std::this_thread::get_id(),
                    std::memory_order_release);
                destroyed_.store(true, std::memory_order_release);
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL QueryInterface(const DasGuid&, void**) override
        {
            return DAS_E_NO_INTERFACE;
        }

        static void ResetTracking() noexcept
        {
            destroyed_.store(false, std::memory_order_release);
            destroyed_on_thread_.store(std::thread::id{});
        }

        [[nodiscard]]
        static bool Destroyed() noexcept
        {
            return destroyed_.load(std::memory_order_acquire);
        }

        [[nodiscard]]
        static std::thread::id DestroyedOnThread() noexcept
        {
            return destroyed_on_thread_.load(std::memory_order_acquire);
        }

    private:
        ~DestructionTrackingObject() = default;

        std::atomic<uint32_t>                      ref_count_{0};
        inline static std::atomic<bool>            destroyed_{false};
        inline static std::atomic<std::thread::id> destroyed_on_thread_{};
    };

    ValidatedIPCMessageHeader MakeRemoteRefEventHeader(
        IpcCommandType command_type,
        uint16_t       source_session_id)
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Event)
            .SetControlPlaneCommand(command_type)
            .SetSourceSessionId(source_session_id)
            .SetTargetSessionId(1)
            .SetBodySize(static_cast<uint32_t>(sizeof(ObjectId)))
            .Build();
    }

    std::vector<uint8_t> MakeObjectIdBody(const ObjectId& object_id)
    {
        std::vector<uint8_t> body;
        AppendToBuffer(body, object_id);
        return body;
    }

    // 在 IO 协程域驱动一次 CONTROL_PLANE EVENT（handler 协程内无真实
    // 挂起点，io_context.run() 同步收敛）
    DasResult DispatchRemoteRefEvent(
        IpcRunLoop&                      run_loop,
        RemoteRefHandler&                remote_ref_handler,
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body)
    {
        IpcResponseSender   sender(run_loop);
        ControlPlaneContext context{run_loop, header};
        auto                future = boost::asio::co_spawn(
            run_loop.GetIoContext(),
            remote_ref_handler.HandleMessage(header, body, sender, context),
            boost::asio::use_future);
        if (run_loop.GetIoContext().stopped())
        {
            run_loop.GetIoContext().restart();
        }
        run_loop.GetIoContext().run();
        return future.get();
    }

    // 在独立 IO 线程上驱动一次 CONTROL_PLANE EVENT，返回该线程 id
    // （final Release 线程域断言：析构线程 ≠ IO 线程）
    DasResult DispatchRemoteRefEventOnIoThread(
        IpcRunLoop&                      run_loop,
        RemoteRefHandler&                remote_ref_handler,
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        std::thread::id&                 io_thread_id)
    {
        IpcResponseSender   sender(run_loop);
        ControlPlaneContext context{run_loop, header};
        auto                future = boost::asio::co_spawn(
            run_loop.GetIoContext(),
            remote_ref_handler.HandleMessage(header, body, sender, context),
            boost::asio::use_future);
        if (run_loop.GetIoContext().stopped())
        {
            run_loop.GetIoContext().restart();
        }
        std::thread io_thread([&run_loop]() { run_loop.GetIoContext().run(); });
        io_thread_id = io_thread.get_id();
        const DasResult result = future.get();
        io_thread.join();
        return result;
    }

    // 模拟 BusinessThread 泵：取出 AsyncCallback 消息并执行回调
    void PumpPendingBridgeCallbacks(
        DAS::Core::IPC::IpcMessageQueue<DAS::Core::IPC::InboundMessage>&
                                  inbound_queue,
        std::chrono::milliseconds pump_timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + pump_timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto queued = inbound_queue.TryPop();
            if (!queued.has_value())
            {
                return;
            }
            if (queued->body.size() != sizeof(IDasAsyncCallback*))
            {
                continue;
            }
            IDasAsyncCallback* callback = nullptr;
            std::memcpy(&callback, queued->body.data(), sizeof(callback));
            if (callback == nullptr)
            {
                continue;
            }
            const auto owned = DAS::DasPtr<IDasAsyncCallback>::Attach(callback);
            static_cast<void>(callback->Do());
        }
    }
} // namespace

TEST_F(
    IpcCommandHandlerTest,
    RemoteRefHandler_AddThenReleaseRecyclesPinlessEntry)
{
    auto*    mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(
        object_manager_.RegisterLocalObject(mock, object_id, false),
        DAS_S_OK);
    EXPECT_EQ(object_manager_.GetLocalEntryCount(), 1u);

    const auto add_header = MakeRemoteRefEventHeader(
        IpcCommandType::RemoteAddRef,
        kRemoteRefPeerSessionId);
    EXPECT_EQ(
        DispatchRemoteRefEvent(
            *run_loop_,
            *remote_ref_handler_,
            add_header,
            MakeObjectIdBody(object_id)),
        DAS_S_OK);
    EXPECT_EQ(object_manager_.GetRemoteHolderCount(object_id), 1u);

    const auto release_header = MakeRemoteRefEventHeader(
        IpcCommandType::RemoteRelease,
        kRemoteRefPeerSessionId);
    EXPECT_EQ(
        DispatchRemoteRefEvent(
            *run_loop_,
            *remote_ref_handler_,
            release_header,
            MakeObjectIdBody(object_id)),
        DAS_S_OK);

    // pins==0 且 holders 空 → 条目回收；gen bump 后迟到 Lookup 收 STALE
    EXPECT_EQ(object_manager_.GetLocalEntryCount(), 0u);
    IDasBase* looked_up = nullptr;
    EXPECT_EQ(
        object_manager_.LookupObject(object_id, &looked_up),
        DAS_E_IPC_STALE_OBJECT_HANDLE);
}

TEST_F(IpcCommandHandlerTest, RemoteRefHandler_DuplicateAddRefIsIdempotent)
{
    auto*    mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(
        object_manager_.RegisterLocalObject(mock, object_id, false),
        DAS_S_OK);

    const auto add_header = MakeRemoteRefEventHeader(
        IpcCommandType::RemoteAddRef,
        kRemoteRefPeerSessionId);
    const auto object_body = MakeObjectIdBody(object_id);
    ASSERT_EQ(
        DispatchRemoteRefEvent(
            *run_loop_,
            *remote_ref_handler_,
            add_header,
            object_body),
        DAS_S_OK);
    ASSERT_EQ(
        DispatchRemoteRefEvent(
            *run_loop_,
            *remote_ref_handler_,
            add_header,
            object_body),
        DAS_S_OK);

    // (oid, session) set 语义：重复 ADD_REF 计数不涨
    EXPECT_EQ(object_manager_.GetRemoteHolderCount(object_id), 1u);
}

TEST_F(
    IpcCommandHandlerTest,
    RemoteRefHandler_ReleaseUnknownEntryOrHolderWarnsAndSucceeds)
{
    // 条目不存在：WARN + 忽略，不崩溃不重建条目（D-14 known-edge）
    const ObjectId unknown_id{
        .session_id = 1,
        .generation = 1,
        .local_id = 777};
    const auto release_header = MakeRemoteRefEventHeader(
        IpcCommandType::RemoteRelease,
        kRemoteRefPeerSessionId);
    EXPECT_EQ(
        DispatchRemoteRefEvent(
            *run_loop_,
            *remote_ref_handler_,
            release_header,
            MakeObjectIdBody(unknown_id)),
        DAS_S_OK);
    EXPECT_EQ(object_manager_.GetLocalEntryCount(), 0u);

    // 条目存在但该 session 不是 holder：幂等 no-op，条目不动
    auto*    mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(
        object_manager_.RegisterLocalObject(mock, object_id, false),
        DAS_S_OK);
    EXPECT_EQ(
        DispatchRemoteRefEvent(
            *run_loop_,
            *remote_ref_handler_,
            release_header,
            MakeObjectIdBody(object_id)),
        DAS_S_OK);
    EXPECT_EQ(object_manager_.GetLocalEntryCount(), 1u);
    EXPECT_EQ(object_manager_.GetRemoteHolderCount(object_id), 0u);
}

TEST_F(IpcCommandHandlerTest, RemoteRefHandler_TruncatedBodyIsDropped)
{
    auto*    mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(
        object_manager_.RegisterLocalObject(mock, object_id, false),
        DAS_S_OK);

    const auto add_header = MakeRemoteRefEventHeader(
        IpcCommandType::RemoteAddRef,
        kRemoteRefPeerSessionId);
    const std::vector<uint8_t> truncated_body(sizeof(ObjectId) - 1, 0xAB);
    EXPECT_EQ(
        DispatchRemoteRefEvent(
            *run_loop_,
            *remote_ref_handler_,
            add_header,
            truncated_body),
        DAS_S_OK);

    // 尺寸不足：丢弃 + WARN，不读越界、零记账
    EXPECT_EQ(object_manager_.GetRemoteHolderCount(object_id), 0u);
}

TEST_F(
    IpcCommandHandlerTest,
    RemoteRefHandler_FinalReleaseBridgedToBusinessThread)
{
    DestructionTrackingObject::ResetTracking();
    auto*    object = new DestructionTrackingObject();
    ObjectId object_id{};
    ASSERT_EQ(
        object_manager_.RegisterLocalObject(object, object_id, false),
        DAS_S_OK);

    std::thread::id io_thread_id{};

    const auto add_header = MakeRemoteRefEventHeader(
        IpcCommandType::RemoteAddRef,
        kRemoteRefPeerSessionId);
    ASSERT_EQ(
        DispatchRemoteRefEventOnIoThread(
            *run_loop_,
            *remote_ref_handler_,
            add_header,
            MakeObjectIdBody(object_id),
            io_thread_id),
        DAS_S_OK);

    // IO 域 RELEASE：条目移除，对象本体经回调桥接 BT——不在 IO 调用栈内
    // 析构
    const auto release_header = MakeRemoteRefEventHeader(
        IpcCommandType::RemoteRelease,
        kRemoteRefPeerSessionId);
    EXPECT_EQ(
        DispatchRemoteRefEventOnIoThread(
            *run_loop_,
            *remote_ref_handler_,
            release_header,
            MakeObjectIdBody(object_id),
            io_thread_id),
        DAS_S_OK);
    EXPECT_FALSE(DestructionTrackingObject::Destroyed());

    // 模拟 BT 泵：回调在泵线程上执行，对象析构随之发生
    PumpPendingBridgeCallbacks(inbound_queue_, std::chrono::milliseconds(100));
    EXPECT_TRUE(DestructionTrackingObject::Destroyed());
    EXPECT_EQ(
        DestructionTrackingObject::DestroyedOnThread(),
        std::this_thread::get_id());
    EXPECT_NE(DestructionTrackingObject::DestroyedOnThread(), io_thread_id);
}

TEST_F(
    IpcCommandHandlerTest,
    RemoteRefHandler_PostFailureAfterQueueShutdownIsSafe)
{
    DestructionTrackingObject::ResetTracking();
    auto*    object = new DestructionTrackingObject();
    ObjectId object_id{};
    ASSERT_EQ(
        object_manager_.RegisterLocalObject(object, object_id, false),
        DAS_S_OK);

    const auto add_header = MakeRemoteRefEventHeader(
        IpcCommandType::RemoteAddRef,
        kRemoteRefPeerSessionId);
    ASSERT_EQ(
        DispatchRemoteRefEvent(
            *run_loop_,
            *remote_ref_handler_,
            add_header,
            MakeObjectIdBody(object_id)),
        DAS_S_OK);

    // 停机：队列反初始化后 push 必失败
    inbound_queue_.Uninitialize();

    const auto release_header = MakeRemoteRefEventHeader(
        IpcCommandType::RemoteRelease,
        kRemoteRefPeerSessionId);
    EXPECT_EQ(
        DispatchRemoteRefEvent(
            *run_loop_,
            *remote_ref_handler_,
            release_header,
            MakeObjectIdBody(object_id)),
        DAS_S_OK);

    // push 失败（停机窗口）不崩溃：回调与其捕获的对象引用被有意
    // 泄漏到进程退出，绝不在本线程析构（线程模型不变量：真实对象
    // 析构不落在 IO 线程）
    EXPECT_FALSE(DestructionTrackingObject::Destroyed());
}

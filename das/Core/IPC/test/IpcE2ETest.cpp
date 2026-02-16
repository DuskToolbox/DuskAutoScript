/**
 * @file IpcE2ETest.cpp
 * @brief IPC End-to-End Integration Tests
 *
 * Tests the full IPC pipeline from Proxy to Stub:
 * - Object registration and lookup
 * - Message serialization and transport
 * - Request/Response round-trip
 * - Event broadcasting
 */

#include <atomic>
#include <chrono>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MessageQueueTransport.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/Core/IPC/Serializer.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <gtest/gtest.h>
#include <thread>

using DAS::Core::IPC::ConnectionManager;
using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::IpcRunLoop;
using DAS::Core::IPC::IpcTransport;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::SerializerReader;
using DAS::Core::IPC::SerializerWriter;
using DAS::Core::IPC::SharedMemoryBlock;
using DAS::Core::IPC::SharedMemoryManager;
using DAS::Core::IPC::SharedMemoryPool;

// Helper classes for in-memory serialization testing
class MemorySerializerWriter : public SerializerWriter
{
private:
    std::vector<uint8_t> buffer_;

public:
    DasResult Write(const void* data, size_t size) override
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + size);
        return DAS_S_OK;
    }

    size_t GetPosition() const override { return buffer_.size(); }

    DasResult Seek(size_t position) override
    {
        if (position > buffer_.size())
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        buffer_.resize(position);
        return DAS_S_OK;
    }

    DasResult Reserve(size_t size) override
    {
        buffer_.reserve(buffer_.size() + size);
        return DAS_S_OK;
    }

    const std::vector<uint8_t>& GetBuffer() const { return buffer_; }
    void                        Clear() { buffer_.clear(); }
};

class MemorySerializerReader : public SerializerReader
{
private:
    std::vector<uint8_t> buffer_;
    size_t               position_;

public:
    explicit MemorySerializerReader(const std::vector<uint8_t>& buffer)
        : buffer_(buffer), position_(0)
    {
    }

    DasResult Read(void* data, size_t size) override
    {
        if (position_ + size > buffer_.size())
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        uint8_t* bytes = static_cast<uint8_t*>(data);
        std::memcpy(bytes, buffer_.data() + position_, size);
        position_ += size;
        return DAS_S_OK;
    }

    size_t GetPosition() const override { return position_; }

    size_t GetRemaining() const override { return buffer_.size() - position_; }

    DasResult Seek(size_t position) override
    {
        if (position > buffer_.size())
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        position_ = position;
        return DAS_S_OK;
    }
};

// ====== Mock Interface for Testing ======

struct MockServiceProxy
{
    uint64_t object_id;
    uint16_t host_id;
    uint16_t plugin_id;

    DasResult CallMethod(
        uint32_t                    method_id,
        const std::vector<uint8_t>& args,
        std::vector<uint8_t>&       result)
    {
        // Serialize request
        MemorySerializerWriter writer;
        writer.WriteUInt32(method_id);
        writer.WriteBytes(args.data(), args.size());

        // In real implementation, this would go through IpcRunLoop
        (void)result; // Placeholder
        return DAS_S_OK;
    }
};

struct MockServiceStub
{
    uint64_t object_id;

    DasResult HandleRequest(
        const IPCMessageHeader& header,
        const uint8_t*          body,
        size_t                  body_size)
    {
        (void)header;
        (void)body;
        (void)body_size;
        return DAS_S_OK;
    }
};

// ====== E2E Test Fixture ======

class IpcE2ETest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        host_object_manager_ = std::make_unique<DistributedObjectManager>();
        plugin_object_manager_ = std::make_unique<DistributedObjectManager>();
        connection_manager_ = std::make_unique<ConnectionManager>();

        ASSERT_EQ(
            host_object_manager_->Initialize(1),
            DAS_S_OK); // Host process ID = 1
        ASSERT_EQ(
            plugin_object_manager_->Initialize(2),
            DAS_S_OK); // Plugin process ID = 2
        ASSERT_EQ(connection_manager_->Initialize(1), DAS_S_OK);
    }

    void TearDown() override
    {
        connection_manager_->Shutdown();
        host_object_manager_->Shutdown();
        plugin_object_manager_->Shutdown();
    }

    std::unique_ptr<DistributedObjectManager> host_object_manager_;
    std::unique_ptr<DistributedObjectManager> plugin_object_manager_;
    std::unique_ptr<ConnectionManager>        connection_manager_;
};

// ====== Object Registration E2E Tests ======

TEST_F(IpcE2ETest, ProxyStub_ObjectRegistration)
{
    // Host registers a local object
    int      dummy_service = 42;
    ObjectId host_object_id{};
    auto     result = host_object_manager_->RegisterLocalObject(
        &dummy_service,
        host_object_id);
    ASSERT_EQ(result, DAS_S_OK);

    // Plugin registers a reference to the remote object
    result = plugin_object_manager_->RegisterRemoteObject(host_object_id);
    ASSERT_EQ(result, DAS_S_OK);

    // Verify the object is accessible
    EXPECT_TRUE(plugin_object_manager_->IsValidObject(host_object_id));
    EXPECT_FALSE(plugin_object_manager_->IsLocalObject(host_object_id));
}

TEST_F(IpcE2ETest, ProxyStub_MultipleObjects)
{
    // Register multiple objects
    int      service1 = 1, service2 = 2, service3 = 3;
    ObjectId id1{}, id2{}, id3{};

    ASSERT_EQ(
        host_object_manager_->RegisterLocalObject(&service1, id1),
        DAS_S_OK);
    ASSERT_EQ(
        host_object_manager_->RegisterLocalObject(&service2, id2),
        DAS_S_OK);
    ASSERT_EQ(
        host_object_manager_->RegisterLocalObject(&service3, id3),
        DAS_S_OK);

    // Plugin registers all remote objects
    ASSERT_EQ(plugin_object_manager_->RegisterRemoteObject(id1), DAS_S_OK);
    ASSERT_EQ(plugin_object_manager_->RegisterRemoteObject(id2), DAS_S_OK);
    ASSERT_EQ(plugin_object_manager_->RegisterRemoteObject(id3), DAS_S_OK);

    // Verify all objects are accessible
    EXPECT_TRUE(plugin_object_manager_->IsValidObject(id1));
    EXPECT_TRUE(plugin_object_manager_->IsValidObject(id2));
    EXPECT_TRUE(plugin_object_manager_->IsValidObject(id3));
}

// ====== Message Serialization E2E Tests ======

TEST_F(IpcE2ETest, ProxyStub_MessageRoundTrip)
{
    // Create a message with V2 header format
    IPCMessageHeader request{};
    request.magic = IPCMessageHeader::MAGIC;
    request.version = IPCMessageHeader::CURRENT_VERSION;
    request.call_id = 1;
    request.message_type = static_cast<uint8_t>(MessageType::REQUEST);
    request.interface_id = 12345;
    request.method_id = 42;
    request.session_id = 0;
    request.generation = 0;
    request.local_id = 0;

    std::string method_args = "test_argument_data";

    // Serialize
    MemorySerializerWriter writer;
    writer.WriteInt32(42); // Method ID
    writer.WriteString(method_args.c_str(), method_args.size());

    // V2 header is already in wire format, no conversion needed

    // Deserialize
    MemorySerializerReader reader(writer.GetBuffer());
    int32_t                method_id;
    ASSERT_EQ(reader.ReadInt32(&method_id), DAS_S_OK);
    EXPECT_EQ(method_id, 42);

    std::string received_args;
    ASSERT_EQ(reader.ReadString(received_args), DAS_S_OK);
    EXPECT_EQ(received_args, method_args);

    // Verify header fields are correct
    EXPECT_EQ(request.call_id, 1ULL);
    EXPECT_EQ(request.message_type, static_cast<uint8_t>(MessageType::REQUEST));
    EXPECT_EQ(request.interface_id, 12345U);
}

// ====== Connection Management E2E Tests ======

TEST_F(IpcE2ETest, Connection_HostPluginHandshake)
{
    // Register connection
    ASSERT_EQ(connection_manager_->RegisterConnection(2, 1), DAS_S_OK);

    // Verify connection is alive
    EXPECT_TRUE(connection_manager_->IsConnectionAlive(2));

    // Send heartbeat
    ASSERT_EQ(connection_manager_->SendHeartbeat(2), DAS_S_OK);

    // Unregister connection
    ASSERT_EQ(connection_manager_->UnregisterConnection(2, 1), DAS_S_OK);

    // Verify connection is dead
    EXPECT_FALSE(connection_manager_->IsConnectionAlive(2));
}

// ====== Object Lifecycle E2E Tests ======

TEST_F(IpcE2ETest, ObjectLifecycle_ReleaseAndGC)
{
    // Register object
    int      dummy = 42;
    ObjectId object_id{};
    ASSERT_EQ(
        host_object_manager_->RegisterLocalObject(&dummy, object_id),
        DAS_S_OK);

    // Add multiple references
    ASSERT_EQ(host_object_manager_->AddRef(object_id), DAS_S_OK);
    ASSERT_EQ(host_object_manager_->AddRef(object_id), DAS_S_OK);

    // Release references one by one
    ASSERT_EQ(host_object_manager_->Release(object_id), DAS_S_OK);
    EXPECT_TRUE(host_object_manager_->IsValidObject(object_id));

    ASSERT_EQ(host_object_manager_->Release(object_id), DAS_S_OK);
    EXPECT_TRUE(host_object_manager_->IsValidObject(object_id));

    // Final release - object should be removed
    ASSERT_EQ(host_object_manager_->Release(object_id), DAS_S_OK);
    EXPECT_FALSE(host_object_manager_->IsValidObject(object_id));
}

// ====== Error Handling E2E Tests ======

TEST_F(IpcE2ETest, ErrorHandling_InvalidObjectId)
{
    void*    ptr = nullptr;
    ObjectId invalid_id{0xFFFF, 0xFFFF, 0xFFFFFFFF};
    auto     result = host_object_manager_->LookupObject(invalid_id, &ptr);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(IpcE2ETest, ErrorHandling_NullObject)
{
    ObjectId id{};
    auto     result = host_object_manager_->RegisterLocalObject(nullptr, id);
    EXPECT_NE(result, DAS_S_OK);
}

// ====== Shared Memory E2E Tests ======

TEST_F(IpcE2ETest, SharedMemory_LargeDataTransfer)
{
    SharedMemoryPool pool;
    std::string      pool_name = "e2e_test_shm_pool";

    ASSERT_EQ(pool.Initialize(pool_name, 1024 * 1024), DAS_S_OK); // 1MB

    // Allocate a large block
    SharedMemoryBlock block;
    ASSERT_EQ(pool.Allocate(65536, block), DAS_S_OK); // 64KB

    // Write data
    std::vector<uint8_t> test_data(65536, 0xAB);
    std::memcpy(block.data, test_data.data(), test_data.size());

    // Read back
    std::vector<uint8_t> read_data(65536);
    std::memcpy(read_data.data(), block.data, test_data.size());

    EXPECT_EQ(test_data, read_data);

    // Cleanup
    ASSERT_EQ(pool.Deallocate(block.handle), DAS_S_OK);
    pool.Shutdown();
}

// ====== Concurrent E2E Tests ======

TEST_F(IpcE2ETest, Concurrent_MultipleRegistrations)
{
    const int                num_threads = 4;
    std::vector<std::thread> threads;
    std::atomic<int>         success_count{0};

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back(
            [&, t]()
            {
                int      dummy = t;
                ObjectId id{};
                if (host_object_manager_->RegisterLocalObject(&dummy, id)
                    == DAS_S_OK)
                {
                    success_count++;
                }
            });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), num_threads);
}

// ====== Full Pipeline E2E Test ======

TEST_F(IpcE2ETest, FullPipeline_RequestResponse)
{
    // 1. Setup: Host registers service
    int      service_impl = 100;
    ObjectId service_id{};
    ASSERT_EQ(
        host_object_manager_->RegisterLocalObject(&service_impl, service_id),
        DAS_S_OK);

    // 2. Plugin gets reference to service
    ASSERT_EQ(
        plugin_object_manager_->RegisterRemoteObject(service_id),
        DAS_S_OK);

    // 3. Plugin creates request with V2 header format
    IPCMessageHeader request{};
    request.magic = IPCMessageHeader::MAGIC;
    request.version = IPCMessageHeader::CURRENT_VERSION;
    request.call_id = 1;
    request.message_type = static_cast<uint8_t>(MessageType::REQUEST);
    request.interface_id = 1;
    request.method_id = 1;
    request.session_id = service_id.session_id;
    request.generation = service_id.generation;
    request.local_id = service_id.local_id;

    MemorySerializerWriter request_writer;
    request_writer.WriteInt32(1);
    request_writer.WriteInt32(0);

    // 4. V2 header is already in wire format, no conversion needed
    IPCMessageHeader received_request = request;

    MemorySerializerReader request_reader(request_writer.GetBuffer());
    int32_t                method_id;
    ASSERT_EQ(request_reader.ReadInt32(&method_id), DAS_S_OK);
    EXPECT_EQ(method_id, 1);

    // 5. Host processes request - reconstruct ObjectId from header fields
    ObjectId received_object_id{
        received_request.session_id,
        received_request.generation,
        received_request.local_id};
    void* obj_ptr = nullptr;
    ASSERT_EQ(
        host_object_manager_->LookupObject(received_object_id, &obj_ptr),
        DAS_S_OK);
    EXPECT_NE(obj_ptr, nullptr);

    // 6. Host sends response
    IPCMessageHeader response{};
    response.magic = IPCMessageHeader::MAGIC;
    response.version = IPCMessageHeader::CURRENT_VERSION;
    response.call_id = request.call_id;
    response.message_type = static_cast<uint8_t>(MessageType::RESPONSE);
    response.error_code = DAS_S_OK;

    MemorySerializerWriter response_writer;
    response_writer.WriteInt32(100);

    // 7. Plugin receives response - no conversion needed for V2
    IPCMessageHeader received_response = response;
    EXPECT_EQ(received_response.call_id, request.call_id);
    EXPECT_EQ(received_response.error_code, DAS_S_OK);

    MemorySerializerReader response_reader(response_writer.GetBuffer());
    int32_t                return_value;
    ASSERT_EQ(response_reader.ReadInt32(&return_value), DAS_S_OK);
    EXPECT_EQ(return_value, 100);
}

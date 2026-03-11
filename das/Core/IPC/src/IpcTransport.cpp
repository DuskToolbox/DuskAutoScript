#include <boost/date_time/posix_time/posix_time.hpp>
#include <cstdint>
#include <cstring>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcTransport.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>
#include <string>
#include <vector>
DAS_CORE_IPC_NS_BEGIN
constexpr uint16_t kFlagLargeMessage = 0x01;

struct IpcTransport::Impl
{
    std::unique_ptr<boost::interprocess::message_queue> host_queue_;
    std::unique_ptr<boost::interprocess::message_queue> plugin_queue_;
    SharedMemoryPool*                                   shm_pool_{nullptr};
    uint32_t                                            max_message_size_;
    uint32_t                                            max_messages_;
    std::string                                         host_queue_name_;
    std::string                                         plugin_queue_name_;
    bool                                                initialized_{false};
    bool                                                is_server_{false};
};

IpcTransport::IpcTransport() : impl_(std::make_unique<Impl>()) {}

IpcTransport::IpcTransport(
    const std::string& host_queue_name,
    const std::string& plugin_queue_name,
    uint32_t           max_message_size,
    uint32_t           max_messages,
    bool               is_server)
: impl_(std::make_unique<Impl>())
{
    impl_->host_queue_name_ = host_queue_name;
    impl_->plugin_queue_name_ = plugin_queue_name;
    impl_->max_message_size_ = max_message_size;
    impl_->max_messages_ = max_messages;
    impl_->is_server_ = is_server;

    try
    {
        if (is_server)
        {
            boost::interprocess::message_queue::remove(host_queue_name.c_str());
            boost::interprocess::message_queue::remove(plugin_queue_name.c_str());

            impl_->host_queue_ =
                std::make_unique<boost::interprocess::message_queue>(
                    boost::interprocess::create_only,
                    host_queue_name.c_str(),
                    max_messages,
                    max_message_size);

            impl_->plugin_queue_ =
                std::make_unique<boost::interprocess::message_queue>(
                    boost::interprocess::create_only,
                    plugin_queue_name.c_str(),
                    max_messages,
                    max_message_size);
        }
        else
        {
            // 客户端角色：交换队列方向
            // host_queue_ 用于 Send，应指向 M2H（Main -> Host）
            // plugin_queue_ 用于 Receive，应指向 H2M（Host -> Main）
            impl_->host_queue_ =
                std::make_unique<boost::interprocess::message_queue>(
                    boost::interprocess::open_only,
                    plugin_queue_name.c_str()); // M2H - 客户端发送用

            impl_->plugin_queue_ =
                std::make_unique<boost::interprocess::message_queue>(
                    boost::interprocess::open_only,
                    host_queue_name.c_str()); // H2M - 客户端接收用

            // 从已存在的队列获取配置
            impl_->max_message_size_ =
                static_cast<uint32_t>(impl_->host_queue_->get_max_msg_size());
            impl_->max_messages_ =
                static_cast<uint32_t>(impl_->host_queue_->get_max_msg());
        }

        impl_->initialized_ = true;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_EXCEPTION(e);
        // 初始化失败，impl_ 仍然有效但未初始化
    }
}

IpcTransport::~IpcTransport() { Uninitialize(); }

DAS::Utils::Expected<std::unique_ptr<IpcTransport>> IpcTransport::Create(
    const std::string& host_queue_name,
    const std::string& plugin_queue_name,
    uint32_t           max_message_size,
    uint32_t           max_messages)
{
    auto transport = std::unique_ptr<IpcTransport>(new IpcTransport(
        host_queue_name,
        plugin_queue_name,
        max_message_size,
        max_messages,
        true));

    if (!transport->impl_->initialized_)
    {
        return DAS::Utils::MakeUnexpected(DAS_E_IPC_MESSAGE_QUEUE_FAILED);
    }

    return transport;
}

DAS::Utils::Expected<std::unique_ptr<IpcTransport>> IpcTransport::Connect(
    const std::string& host_queue_name,
    const std::string& plugin_queue_name)
{
    auto transport = std::unique_ptr<IpcTransport>(new IpcTransport(
        host_queue_name,
        plugin_queue_name,
        0, // 客户端从队列获取配置
        0,
        false));

    if (!transport->impl_->initialized_)
    {
        return DAS::Utils::MakeUnexpected(DAS_E_IPC_MESSAGE_QUEUE_FAILED);
    }

    return transport;
}

void IpcTransport::Uninitialize()
{
    if (!impl_->initialized_)
    {
        return;
    }

    impl_->host_queue_.reset();
    impl_->plugin_queue_.reset();

    if (impl_->is_server_)
    {
        try
        {
            boost::interprocess::message_queue::remove(
                impl_->host_queue_name_.c_str());
            boost::interprocess::message_queue::remove(
                impl_->plugin_queue_name_.c_str());
        }
        catch (...)
        {
        }
    }

    impl_->initialized_ = false;
}

DasResult IpcTransport::Send(
    const ValidatedIPCMessageHeader& header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    if (!impl_->initialized_)
    {
        DAS_CORE_LOG_ERROR("Transport not initialized");
        return DAS_E_IPC_CONNECTION_LOST;
    }

    size_t total_size = sizeof(IPCMessageHeader) + body_size;

    if (total_size <= impl_->max_message_size_)
    {
        return SendSmallMessage(header, body, body_size);
    }
    else
    {
        return SendLargeMessage(header, body, body_size);
    }
}

DasResult IpcTransport::Receive(
    IPCMessageHeader&     out_header,
    std::vector<uint8_t>& out_body,
    uint32_t              timeout_ms)
{
    if (!impl_->initialized_)
    {
        DAS_CORE_LOG_ERROR("Transport not initialized");
        return DAS_E_IPC_CONNECTION_LOST;
    }

    std::vector<uint8_t> buffer(impl_->max_message_size_);
    size_t               received_size = 0;
    unsigned int         priority = 0;

    try
    {
        if (timeout_ms == 0)
        {
            // 无限超时 - 使用阻塞接收
            impl_->plugin_queue_->receive(
                buffer.data(),
                impl_->max_message_size_,
                received_size,
                priority);
        }
        else
        {
            // 有限超时 - 使用带超时的接收
            // NOTE: 使用 universal_time() 以确保与 Windows 兼容
            bool received = impl_->plugin_queue_->timed_receive(
                buffer.data(),
                impl_->max_message_size_,
                received_size,
                priority,
                boost::posix_time::ptime(
                    boost::posix_time::microsec_clock::universal_time()
                    + boost::posix_time::milliseconds(timeout_ms)));
            if (!received)
            {
                DAS_CORE_LOG_WARN("Receive timeout after {} ms", timeout_ms);
                return DAS_E_IPC_TIMEOUT;
            }
        }

        if (received_size < sizeof(IPCMessageHeader))
        {
            DAS_CORE_LOG_ERROR(
                "Message too small ({} bytes, expected at least {} bytes)",
                received_size,
                sizeof(IPCMessageHeader));
            return DAS_E_IPC_INVALID_MESSAGE_HEADER;
        }

        std::memcpy(&out_header, buffer.data(), sizeof(IPCMessageHeader));

        // Validate magic and version
        if (out_header.magic != IPCMessageHeader::MAGIC
            || out_header.version != IPCMessageHeader::CURRENT_VERSION)
        {
            DAS_CORE_LOG_ERROR(
                "Invalid message header (magic = 0x{:08X}, version = {})",
                out_header.magic,
                out_header.version);
            return DAS_E_IPC_INVALID_MESSAGE_HEADER;
        }

        if (out_header.flags & kFlagLargeMessage)
        {
            if (impl_->shm_pool_ == nullptr)
            {
                DAS_CORE_LOG_ERROR(
                    "Large message received but shared memory pool not set");
                return DAS_E_IPC_SHM_FAILED;
            }

            if (received_size < sizeof(IPCMessageHeader) + sizeof(uint64_t))
            {
                DAS_CORE_LOG_ERROR(
                    "Large message handle missing (size = {})",
                    received_size);
                return DAS_E_IPC_INVALID_MESSAGE;
            }

            uint64_t handle;
            std::memcpy(
                &handle,
                buffer.data() + sizeof(IPCMessageHeader),
                sizeof(uint64_t));

            SharedMemoryBlock shm_block;
            auto result = impl_->shm_pool_->GetBlockByHandle(handle, shm_block);
            if (result != DAS_S_OK)
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to get shared memory block for handle = {}",
                    handle);
                return result;
            }

            out_body.resize(shm_block.size);
            std::memcpy(out_body.data(), shm_block.data, shm_block.size);

            impl_->shm_pool_->Deallocate(handle);

            return DAS_S_OK;
        }

        if (received_size > sizeof(IPCMessageHeader))
        {
            out_body.assign(
                buffer.data() + sizeof(IPCMessageHeader),
                buffer.data() + received_size);
        }
        else
        {
            out_body.clear();
        }

        return DAS_S_OK;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_EXCEPTION(e);
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }
}

DasResult IpcTransport::SetSharedMemoryPool(SharedMemoryPool* pool)
{
    impl_->shm_pool_ = pool;
    return DAS_S_OK;
}

bool IpcTransport::IsConnected() const { return impl_->initialized_; }

std::string IpcTransport::MakeQueueName(
    uint32_t main_pid,
    uint32_t host_pid,
    bool     is_main_to_host)
{
    if (is_main_to_host)
    {
        return DAS_FMT_NS::format("das_ipc_{}_{}_m2h", main_pid, host_pid);
    }
    else
    {
        return DAS_FMT_NS::format("das_ipc_{}_{}_h2m", main_pid, host_pid);
    }
}

DasResult IpcTransport::SendSmallMessage(
    const ValidatedIPCMessageHeader& header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    std::vector<uint8_t> buffer(sizeof(IPCMessageHeader) + body_size);

    std::memcpy(buffer.data(), header, header.Size());
    if (body != nullptr && body_size > 0)
    {
        std::memcpy(buffer.data() + header.Size(), body, body_size);
    }

    try
    {
        impl_->host_queue_->send(buffer.data(), buffer.size(), 0);
        return DAS_S_OK;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_EXCEPTION(e);
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }
}

DasResult IpcTransport::SendLargeMessage(
    const ValidatedIPCMessageHeader& header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    if (impl_->shm_pool_ == nullptr)
    {
        DAS_CORE_LOG_ERROR("Shared memory pool not set");
        return DAS_E_IPC_SHM_FAILED;
    }

    SharedMemoryBlock block;
    auto              result = impl_->shm_pool_->Allocate(body_size, block);
    if (result != DAS_S_OK)
    {
        DAS_CORE_LOG_ERROR(
            "Failed to allocate {} bytes in shared memory",
            body_size);
        return result;
    }

    std::memcpy(block.data, body, body_size);

    // 创建带大消息标志的 header
    IPCMessageHeader shm_header = header.Raw();
    shm_header.flags |= kFlagLargeMessage;

    // 使用 Builder 创建新的 validated header
    auto validated_shm_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(static_cast<MessageType>(shm_header.message_type))
            .SetBusinessInterface(shm_header.interface_id, shm_header.method_id)
            .SetBodySize(sizeof(uint64_t))
            .SetCallId(shm_header.call_id)
            .SetObject(
                shm_header.session_id,
                shm_header.generation,
                shm_header.local_id)
            .SetFlags(shm_header.flags)
            .Build();

    std::vector<uint8_t> handle_buffer(sizeof(uint64_t));
    std::memcpy(handle_buffer.data(), &block.handle, sizeof(uint64_t));

    result = SendSmallMessage(
        validated_shm_header,
        handle_buffer.data(),
        sizeof(uint64_t));
    if (result != DAS_S_OK)
    {
        impl_->shm_pool_->Deallocate(block.handle);
    }

    return result;
}
DAS_CORE_IPC_NS_END

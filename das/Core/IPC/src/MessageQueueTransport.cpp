#include <boost/date_time/posix_time/posix_time.hpp>
#include <cstdint>
#include <cstring>
#include <das/Core/IPC/MessageQueueTransport.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <string>
#include <vector>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        constexpr uint16_t kFlagLargeMessage = 0x01;

        struct IpcTransport::Impl
        {
            std::unique_ptr<boost::interprocess::message_queue> host_queue_;
            std::unique_ptr<boost::interprocess::message_queue> plugin_queue_;
            SharedMemoryPool* shm_pool_{nullptr};
            uint32_t          max_message_size_;
            uint32_t          max_messages_;
            std::string       host_queue_name_;
            std::string       plugin_queue_name_;
            bool              initialized_{false};
        };

        IpcTransport::IpcTransport() : impl_(std::make_unique<Impl>()) {}

        IpcTransport::~IpcTransport() { Shutdown(); }

        DasResult IpcTransport::Initialize(
            const std::string& host_queue_name,
            const std::string& plugin_queue_name,
            uint32_t           max_message_size,
            uint32_t           max_messages)
        {
            impl_->host_queue_name_ = host_queue_name;
            impl_->plugin_queue_name_ = plugin_queue_name;
            impl_->max_message_size_ = max_message_size;
            impl_->max_messages_ = max_messages;

            try
            {
                boost::interprocess::message_queue::remove(
                    host_queue_name.c_str());
                boost::interprocess::message_queue::remove(
                    plugin_queue_name.c_str());

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

                impl_->initialized_ = true;
                return DAS_S_OK;
            }
            catch (const boost::interprocess::interprocess_exception&)
            {
                return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
            }
        }

        DasResult IpcTransport::Shutdown()
        {
            if (!impl_->initialized_)
            {
                return DAS_S_OK;
            }

            impl_->host_queue_.reset();
            impl_->plugin_queue_.reset();

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

            impl_->initialized_ = false;
            return DAS_S_OK;
        }

        DasResult IpcTransport::Send(
            const IPCMessageHeader& header,
            const uint8_t*          body,
            size_t                  body_size)
        {
            if (!impl_->initialized_)
            {
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
                return DAS_E_IPC_CONNECTION_LOST;
            }

            std::vector<uint8_t> buffer(impl_->max_message_size_);
            size_t               received_size = 0;
            unsigned int         priority = 0;

            try
            {
                bool received = impl_->plugin_queue_->timed_receive(
                    buffer.data(),
                    impl_->max_message_size_,
                    received_size,
                    priority,
                    boost::posix_time::ptime(
                        boost::posix_time::microsec_clock::local_time()
                        + boost::posix_time::milliseconds(timeout_ms)));

                if (!received)
                {
                    return DAS_E_IPC_TIMEOUT;
                }

                if (received_size < sizeof(IPCMessageHeader))
                {
                    return DAS_E_IPC_INVALID_MESSAGE_HEADER;
                }

                std::memcpy(
                    &out_header,
                    buffer.data(),
                    sizeof(IPCMessageHeader));

                // 验证 magic 和 version
                if (out_header.magic != IPCMessageHeader::MAGIC
                    || out_header.version != IPCMessageHeader::CURRENT_VERSION)
                {
                    return DAS_E_IPC_INVALID_MESSAGE_HEADER;
                }

                if (out_header.flags & kFlagLargeMessage)
                {
                    if (impl_->shm_pool_ == nullptr)
                    {
                        return DAS_E_IPC_SHM_FAILED;
                    }

                    if (received_size
                        < sizeof(IPCMessageHeader) + sizeof(uint64_t))
                    {
                        return DAS_E_IPC_INVALID_MESSAGE;
                    }

                    uint64_t handle;
                    std::memcpy(
                        &handle,
                        buffer.data() + sizeof(IPCMessageHeader),
                        sizeof(uint64_t));

                    SharedMemoryBlock shm_block;
                    auto              result =
                        impl_->shm_pool_->GetBlockByHandle(handle, shm_block);
                    if (result != DAS_S_OK)
                    {
                        return result;
                    }

                    out_body.resize(shm_block.size);
                    std::memcpy(
                        out_body.data(),
                        shm_block.data,
                        shm_block.size);

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
            catch (const boost::interprocess::interprocess_exception&)
            {
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
            uint16_t host_id,
            uint16_t plugin_id,
            bool     is_host_to_plugin)
        {
            if (is_host_to_plugin)
            {
                return "das_ipc_" + std::to_string(host_id) + "_"
                       + std::to_string(plugin_id) + "_h2p";
            }
            else
            {
                return "das_ipc_" + std::to_string(host_id) + "_"
                       + std::to_string(plugin_id) + "_p2h";
            }
        }

        DasResult IpcTransport::SendSmallMessage(
            const IPCMessageHeader& header,
            const uint8_t*          body,
            size_t                  body_size)
        {
            std::vector<uint8_t> buffer(sizeof(IPCMessageHeader) + body_size);

            std::memcpy(buffer.data(), &header, sizeof(IPCMessageHeader));
            if (body != nullptr && body_size > 0)
            {
                std::memcpy(
                    buffer.data() + sizeof(IPCMessageHeader),
                    body,
                    body_size);
            }

            try
            {
                impl_->host_queue_->send(buffer.data(), buffer.size(), 0);
                return DAS_S_OK;
            }
            catch (const boost::interprocess::interprocess_exception&)
            {
                return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
            }
        }

        DasResult IpcTransport::SendLargeMessage(
            const IPCMessageHeader& header,
            const uint8_t*          body,
            size_t                  body_size)
        {
            if (impl_->shm_pool_ == nullptr)
            {
                return DAS_E_IPC_SHM_FAILED;
            }

            SharedMemoryBlock block;
            auto result = impl_->shm_pool_->Allocate(body_size, block);
            if (result != DAS_S_OK)
            {
                return result;
            }

            std::memcpy(block.data, body, body_size);

            IPCMessageHeader shm_header = header;
            shm_header.flags |= kFlagLargeMessage;

            std::vector<uint8_t> handle_buffer(sizeof(uint64_t));
            std::memcpy(handle_buffer.data(), &block.handle, sizeof(uint64_t));

            result = SendSmallMessage(
                shm_header,
                handle_buffer.data(),
                sizeof(uint64_t));
            if (result != DAS_S_OK)
            {
                impl_->shm_pool_->Deallocate(block.handle);
            }

            return result;
        }
    }
}
DAS_NS_END

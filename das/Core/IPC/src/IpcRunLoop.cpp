#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MessageQueueTransport.h>
#include <mutex>
#include <thread>
#include <unordered_map>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        struct IpcRunLoop::Impl
        {
            std::unordered_map<uint64_t, NestedCallContext> pending_calls_;
            std::unique_ptr<IpcTransport>                   transport_;
            RequestHandler                                  request_handler_;
            std::atomic<uint64_t>                           next_call_id_{1};
            std::atomic<bool>                               running_{false};
            std::thread                                     io_thread_;
            std::mutex                                      pending_mutex_;
            std::condition_variable                         pending_cv_;
            uint32_t                                        nested_depth_{0};
            static constexpr uint32_t MAX_NESTED_DEPTH = 32;
        };

        IpcRunLoop::IpcRunLoop() : impl_(std::make_unique<Impl>()) {}

        IpcRunLoop::~IpcRunLoop() { Stop(); }

        DasResult IpcRunLoop::Initialize()
        {
            impl_->transport_ = std::make_unique<IpcTransport>();
            return DAS_S_OK;
        }

        DasResult IpcRunLoop::Shutdown()
        {
            Stop();
            impl_->transport_.reset();
            return DAS_S_OK;
        }

        DasResult IpcRunLoop::Run()
        {
            if (impl_->running_.load())
            {
                return DAS_E_IPC_DEADLOCK_DETECTED;
            }

            impl_->running_.store(true);
            impl_->io_thread_ = std::thread([this]() { this->RunInternal(); });

            return DAS_S_OK;
        }

        DasResult IpcRunLoop::Stop()
        {
            if (!impl_->running_.load())
            {
                return DAS_S_OK;
            }

            impl_->running_.store(false);

            {
                std::unique_lock<std::mutex> lock(impl_->pending_mutex_);

                for (auto& [call_id, ctx] : impl_->pending_calls_)
                {
                    ctx.completed = true;
                }
                impl_->pending_cv_.notify_all();
            }

            if (impl_->io_thread_.joinable())
            {
                impl_->io_thread_.join();
            }

            return DAS_S_OK;
        }

        void IpcRunLoop::SetRequestHandler(RequestHandler handler)
        {
            impl_->request_handler_ = std::move(handler);
        }

        DasResult IpcRunLoop::SendRequest(
            const IPCMessageHeader& request_header,
            const uint8_t*          body,
            size_t                  body_size,
            std::vector<uint8_t>&   response_body)
        {
            if (impl_->nested_depth_ >= Impl::MAX_NESTED_DEPTH)
            {
                return DAS_E_IPC_DEADLOCK_DETECTED;
            }

            uint64_t         call_id = impl_->next_call_id_.fetch_add(1);
            IPCMessageHeader header = request_header;
            header.call_id = call_id;
            header.message_type = MessageType::REQUEST;

            {
                std::unique_lock<std::mutex> lock(impl_->pending_mutex_);
                impl_->pending_calls_[call_id] =
                    NestedCallContext{.call_id = call_id, .completed = false};
            }

            auto send_result = impl_->transport_->Send(header, body, body_size);
            if (send_result != DAS_S_OK)
            {
                std::unique_lock<std::mutex> lock(impl_->pending_mutex_);
                impl_->pending_calls_.erase(call_id);
                return send_result;
            }

            impl_->nested_depth_++;

            {
                std::unique_lock<std::mutex> lock(impl_->pending_mutex_);
                auto it = impl_->pending_calls_.find(call_id);

                if (it != impl_->pending_calls_.end())
                {
                    impl_->pending_cv_.wait(
                        lock,
                        [&, this]
                        {
                            return it->second.completed
                                   || !impl_->running_.load();
                        });

                    if (!it->second.completed)
                    {
                        impl_->pending_calls_.erase(it);
                        impl_->nested_depth_--;
                        return DAS_E_IPC_TIMEOUT;
                    }

                    response_body = std::move(it->second.response_buffer);
                    impl_->pending_calls_.erase(it);
                }
            }

            impl_->nested_depth_--;
            return DAS_S_OK;
        }

        DasResult IpcRunLoop::SendResponse(
            const IPCMessageHeader& response_header,
            const uint8_t*          body,
            size_t                  body_size)
        {
            IPCMessageHeader header = response_header;
            header.message_type = MessageType::RESPONSE;

            return impl_->transport_->Send(header, body, body_size);
        }

        DasResult IpcRunLoop::SendEvent(
            const IPCMessageHeader& event_header,
            const uint8_t*          body,
            size_t                  body_size)
        {
            IPCMessageHeader header = event_header;
            header.message_type = MessageType::EVENT;

            return impl_->transport_->Send(header, body, body_size);
        }

        bool IpcRunLoop::IsRunning() const { return impl_->running_.load(); }

        void IpcRunLoop::RunInternal()
        {
            while (impl_->running_.load())
            {
                IPCMessageHeader     header;
                std::vector<uint8_t> body;

                auto result = impl_->transport_->Receive(header, body, 100);

                if (result == DAS_E_IPC_TIMEOUT)
                {
                    continue;
                }

                if (result != DAS_S_OK)
                {
                    break;
                }

                ProcessMessage(header, body.data(), body.size());
            }
        }

        DasResult IpcRunLoop::ProcessMessage(
            const IPCMessageHeader& header,
            const uint8_t*          body,
            size_t                  body_size)
        {
            if (header.message_type == MessageType::RESPONSE)
            {
                std::unique_lock<std::mutex> lock(impl_->pending_mutex_);
                auto it = impl_->pending_calls_.find(header.call_id);

                if (it != impl_->pending_calls_.end())
                {
                    it->second.response_buffer.assign(body, body + body_size);
                    it->second.completed = true;
                    impl_->pending_cv_.notify_one();
                }
                else
                {
                }

                return DAS_S_OK;
            }

            if (header.message_type == MessageType::REQUEST)
            {
                if (!impl_->request_handler_)
                {
                    IPCMessageHeader response = header;
                    response.message_type = MessageType::RESPONSE;
                    response.error_code = DAS_E_IPC_INVALID_INTERFACE_ID;

                    impl_->transport_->Send(response, nullptr, 0);
                    return DAS_S_OK;
                }

                auto result = impl_->request_handler_(header, body, body_size);

                return result;
            }

            if (header.message_type == MessageType::EVENT)
            {
                return DAS_S_OK;
            }

            if (header.message_type == MessageType::HEARTBEAT)
            {
                return DAS_S_OK;
            }

            return DAS_E_IPC_INVALID_MESSAGE_TYPE;
        }
    }
}
DAS_NS_END

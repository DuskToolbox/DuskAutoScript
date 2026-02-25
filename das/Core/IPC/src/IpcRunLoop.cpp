#include <atomic>
#include <chrono>
#include <cstdint>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MessageQueueTransport.h>
#include <mutex>
#include <stdexec/execution.hpp>
#include <thread>
#include <unordered_map>
DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        // 线程局部嵌套深度跟踪（用于 re-entrant 调用检测）
        thread_local uint32_t     t_nested_depth = 0;
        static constexpr uint32_t MAX_NESTED_DEPTH = 32;
        static constexpr uint32_t PUMP_POLL_TIMEOUT_MS = 10;

        IpcRunLoop::IpcRunLoop() = default;

        IpcRunLoop::~IpcRunLoop() { Stop(); }

        DasResult IpcRunLoop::Initialize()
        {
            transport_ = std::make_unique<IpcTransport>();
            return DAS_S_OK;
        }

        DasResult IpcRunLoop::Shutdown()
        {
            Stop();
            transport_.reset();
            return DAS_S_OK;
        }

        DasResult IpcRunLoop::Stop()
        {
            if (!running_.load())
            {
                return DAS_S_OK;
            }

            running_.store(false);

            {
                std::unique_lock<std::mutex> lock(pending_mutex_);

                for (auto& [call_id, ctx] : pending_calls_)
                {
                    ctx.completed = true;
                }
            }

            if (io_thread_.joinable())
            {
                io_thread_.join();
            }

            return DAS_S_OK;
        }

        void IpcRunLoop::RegisterHandler(
            std::unique_ptr<IMessageHandler> handler)
        {
            if (handler)
            {
                uint32_t id = handler->GetInterfaceId();
                handlers_[id] = std::move(handler);
            }
        }

        IMessageHandler* IpcRunLoop::GetHandler(uint32_t interface_id) const
        {
            auto it = handlers_.find(interface_id);
            return (it != handlers_.end()) ? it->second.get() : nullptr;
        }

        DasResult IpcRunLoop::DispatchToHandler(
            const IPCMessageHeader&     header,
            const std::vector<uint8_t>& body)
        {
            // 握手消息 interface_id 1-4
            if (header.interface_id >= 1 && header.interface_id <= 4)
            {
                auto* handshake_handler =
                    GetHandler(1); // HandshakeHandler::INTERFACE_ID = 1
                if (handshake_handler)
                {
                    IpcResponseSender sender(*this);
                    return handshake_handler->HandleMessage(
                        header,
                        body,
                        sender);
                }
            }

            // 精确匹配
            auto* handler = GetHandler(header.interface_id);
            if (handler)
            {
                IpcResponseSender sender(*this);
                return handler->HandleMessage(header, body, sender);
            }

            // 回退到 IpcCommandHandler (处理所有命令类型)
            auto* cmd_handler =
                GetHandler(2); // IpcCommandHandler::INTERFACE_ID = 2
            if (cmd_handler)
            {
                IpcResponseSender sender(*this);
                return cmd_handler->HandleMessage(header, body, sender);
            }

            return DAS_E_NOT_FOUND;
        }

        bool IpcRunLoop::ReceiveAndDispatch(std::chrono::milliseconds timeout)
        {
            IPCMessageHeader     header;
            std::vector<uint8_t> body;

            auto result = transport_->Receive(
                header,
                body,
                static_cast<int>(timeout.count()));

            if (result == DAS_S_OK)
            {
                // 1. 优先检查是否是响应消息
                if (header.message_type
                    == static_cast<uint8_t>(MessageType::RESPONSE))
                {
                    std::unique_lock<std::mutex> lock(pending_mutex_);
                    auto it = pending_calls_.find(header.call_id);
                    if (it != pending_calls_.end())
                    {
                        it->second.response_buffer = std::move(body);
                        it->second.completed = true;
                    }
                    return true;
                }

                // 2. 分发到注册的处理器
                DispatchToHandler(header, body);
                return true;
            }
            return false;
        }

        DasResult IpcRunLoop::SendMessage(
            const IPCMessageHeader&   request_header,
            const uint8_t*            body,
            size_t                    body_size,
            std::vector<uint8_t>&     response_body,
            std::chrono::milliseconds timeout)
        {
            if (t_nested_depth >= MAX_NESTED_DEPTH)
            {
                return DAS_E_IPC_DEADLOCK_DETECTED;
            }

            uint64_t         call_id = next_call_id_.fetch_add(1);
            IPCMessageHeader header = request_header;
            header.call_id = call_id;
            header.message_type = static_cast<uint8_t>(MessageType::REQUEST);

            // 创建等待上下文
            {
                std::unique_lock<std::mutex> lock(pending_mutex_);
                pending_calls_[call_id] = NestedCallContext{call_id, {}, false};
            }

            // 发送请求
            auto send_result = transport_->Send(header, body, body_size);
            if (send_result != DAS_S_OK)
            {
                std::unique_lock<std::mutex> lock(pending_mutex_);
                pending_calls_.erase(call_id);
                return send_result;
            }

            t_nested_depth++;
            auto deadline = std::chrono::steady_clock::now() + timeout;

            // 使用 ReceiveAndDispatch 等待响应
            while (std::chrono::steady_clock::now() < deadline)
            {
                // 检查是否已收到响应
                {
                    std::unique_lock<std::mutex> lock(pending_mutex_);
                    auto it = pending_calls_.find(call_id);
                    if (it != pending_calls_.end() && it->second.completed)
                    {
                        response_body = std::move(it->second.response_buffer);
                        pending_calls_.erase(it);
                        t_nested_depth--;
                        return DAS_S_OK;
                    }
                }

                // 使用 ReceiveAndDispatch 处理消息（关键可重入点）
                ReceiveAndDispatch(
                    std::chrono::milliseconds(PUMP_POLL_TIMEOUT_MS));

                if (!running_.load())
                {
                    std::unique_lock<std::mutex> lock(pending_mutex_);
                    pending_calls_.erase(call_id);
                    t_nested_depth--;
                    return DAS_E_IPC_TIMEOUT;
                }
            }

            // 超时
            {
                std::unique_lock<std::mutex> lock(pending_mutex_);
                pending_calls_.erase(call_id);
            }
            t_nested_depth--;
            return DAS_E_IPC_TIMEOUT;
        }

        DasResult IpcRunLoop::SendResponse(
            const IPCMessageHeader& response_header,
            const uint8_t*          body,
            size_t                  body_size)
        {
            IPCMessageHeader header = response_header;
            header.message_type = static_cast<uint8_t>(MessageType::RESPONSE);

            return transport_->Send(header, body, body_size);
        }

        DasResult IpcRunLoop::SendEvent(
            const IPCMessageHeader& event_header,
            const uint8_t*          body,
            size_t                  body_size)
        {
            IPCMessageHeader header = event_header;
            header.message_type = static_cast<uint8_t>(MessageType::EVENT);

            return transport_->Send(header, body, body_size);
        }

        bool IpcRunLoop::IsRunning() const { return running_.load(); }

        void IpcRunLoop::SetTransport(std::unique_ptr<IpcTransport> transport)
        {
            transport_ = std::move(transport);
        }

        IpcTransport* IpcRunLoop::GetTransport() const
        {
            return transport_.get();
        }

        void IpcRunLoop::RunInternal()
        {
            DasResult exit_code = DAS_S_OK;

            while (running_.load())
            {
                IPCMessageHeader     header;
                std::vector<uint8_t> body;

                auto result = transport_->Receive(header, body, 100);

                if (result == DAS_E_IPC_TIMEOUT)
                {
                    continue;
                }

                if (result != DAS_S_OK)
                {
                    exit_code = result;
                    break;
                }

                ProcessMessage(header, body.data(), body.size());
            }

            exit_code_.store(exit_code);
        }

        DasResult IpcRunLoop::ProcessMessage(
            const IPCMessageHeader& header,
            const uint8_t*          body,
            size_t                  body_size)
        {
            if (header.message_type
                == static_cast<uint8_t>(MessageType::RESPONSE))
            {
                std::unique_lock<std::mutex> lock(pending_mutex_);
                auto it = pending_calls_.find(header.call_id);

                if (it != pending_calls_.end())
                {
                    it->second.response_buffer.assign(body, body + body_size);
                    it->second.completed = true;
                }

                return DAS_S_OK;
            }

            if (header.message_type
                == static_cast<uint8_t>(MessageType::REQUEST))
            {
                // 优先使用新处理器系统
                std::vector<uint8_t> body_vec(body, body + body_size);
                auto dispatch_result = DispatchToHandler(header, body_vec);

                // 如果找到处理器，直接返回结果
                if (dispatch_result != DAS_E_NOT_FOUND)
                {
                    return dispatch_result;
                }

                // 没有处理器，发送错误响应
                IPCMessageHeader response = header;
                response.message_type =
                    static_cast<uint8_t>(MessageType::RESPONSE);
                response.error_code = DAS_E_IPC_INVALID_INTERFACE_ID;

                transport_->Send(response, nullptr, 0);
                return DAS_S_OK;
            }

            if (header.message_type == static_cast<uint8_t>(MessageType::EVENT))
            {
                return DAS_S_OK;
            }

            if (header.message_type
                == static_cast<uint8_t>(MessageType::HEARTBEAT))
            {
                return DAS_S_OK;
            }

            return DAS_E_IPC_INVALID_MESSAGE_TYPE;
        }

        DasResult IpcRunLoop::Run()
        {
            if (running_.load())
            {
                return DAS_E_IPC_DEADLOCK_DETECTED;
            }

            running_.store(true);
            exit_code_.store(DAS_S_OK);
            io_thread_ = std::thread([this]() { this->RunInternal(); });

            // 等待线程完成（阻塞）
            if (io_thread_.joinable())
            {
                io_thread_.join();
            }
            return exit_code_.load();
        }

        DasResult IpcRunLoop::WaitForShutdown()
        {
            if (io_thread_.joinable())
            {
                io_thread_.join();
            }
            return exit_code_.load();
        }

        //=========================================================================
        // PostMessage 实现
        //=========================================================================

        void IpcRunLoop::PostMessage(PostedCallback callback)
        {
            {
                std::lock_guard<std::mutex> lock(post_queue_mutex_);
                post_queue_.push(std::move(callback));
            }

            // 发送唤醒消息，让 Receive() 返回
            SendWakeupMessage();
        }

        void IpcRunLoop::SendWakeupMessage()
        {
            if (!transport_)
            {
                return;
            }

            // 构造唤醒消息（控制平面，interface_id=5）
            IPCMessageHeader header{};
            header.magic = IPCMessageHeader::MAGIC;
            header.version = IPCMessageHeader::CURRENT_VERSION;
            header.session_id = 0; // 控制平面
            header.generation = 0;
            header.local_id = 0;
            header.interface_id = static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_WAKEUP);
            header.body_size = 0;

            transport_->Send(header, nullptr, 0);
        }

        void IpcRunLoop::ProcessPostedCallbacks()
        {
            std::queue<PostedCallback> local_queue;

            {
                std::lock_guard<std::mutex> lock(post_queue_mutex_);
                local_queue = std::move(post_queue_);
                post_queue_ = std::queue<PostedCallback>();
            }

            while (!local_queue.empty())
            {
                auto callback = std::move(local_queue.front());
                local_queue.pop();
                callback(); // 执行
            }
        }

        void IpcRunLoop::PostStartHost(
            const std::string&                       plugin_path,
            std::function<void(DasResult, uint16_t)> on_complete)
        {
            // Wave 5 会实现真正的 HostLauncher
            // 这里先预留接口
            PostMessage(
                [this, plugin_path, on_complete]()
                {
                    // TODO: 实现 HostLauncher
                    if (on_complete)
                    {
                        on_complete(DAS_E_NO_IMPLEMENTATION, 0);
                    }
                });
}
        void IpcRunLoop::PostStopHost(uint16_t session_id)
        {
            PostMessage(
                [this, session_id]()
                {
                    // TODO: 实现 HostLauncher
                    (void)session_id;
                });
        }

    }
}
DAS_NS_END

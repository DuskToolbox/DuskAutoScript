#include <chrono>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/IpcErrors.h>
#include <mutex>
#include <unordered_map>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        // ============================================================
        // Host 端握手管理器
        // ============================================================

        /**
         * @brief Host 端握手管理器
         *
         * 负责处理来自 Child 进程的握手请求，分配 session_id。
         */
        class HostHandshakeManager
        {
        public:
            HostHandshakeManager() = default;
            ~HostHandshakeManager() = default;

            static constexpr uint16_t INVALID_SESSION_ID = 0;
            static constexpr uint16_t MIN_SESSION_ID = 1;
            static constexpr uint16_t MAX_SESSION_ID = 0xFFFE;

            DasResult ProcessHelloRequest(
                const HelloRequestV1& request,
                WelcomeResponseV1&    response,
                uint16_t              host_session_id)
            {
                std::lock_guard<std::mutex> lock(mutex_);

                // 验证协议版本
                if (request.protocol_version
                    != HelloRequestV1::CURRENT_PROTOCOL_VERSION)
                {
                    InitWelcomeResponse(
                        response,
                        INVALID_SESSION_ID,
                        WelcomeResponseV1::STATUS_VERSION_MISMATCH);
                    return DAS_E_IPC_INVALID_MESSAGE;
                }

                // 验证插件名称
                if (request.plugin_name[0] == '\0')
                {
                    InitWelcomeResponse(
                        response,
                        INVALID_SESSION_ID,
                        WelcomeResponseV1::STATUS_INVALID_NAME);
                    return DAS_E_IPC_INVALID_MESSAGE;
                }

                // 分配 session_id
                uint16_t child_session_id = AllocateSessionId();
                if (child_session_id == INVALID_SESSION_ID)
                {
                    InitWelcomeResponse(
                        response,
                        INVALID_SESSION_ID,
                        WelcomeResponseV1::STATUS_TOO_MANY_CLIENTS);
                    return DAS_E_IPC_CONNECTION_LIMIT_REACHED;
                }

                // 记录待确认的连接
                PendingConnection pending;
                pending.child_session_id = child_session_id;
                pending.host_session_id = host_session_id;
                pending.pid = request.pid;
                pending.plugin_name = request.plugin_name;
                pending.state = HandshakeState::WelcomeRecv;
                pending_connections_[child_session_id] = pending;

                InitWelcomeResponse(response, child_session_id);
                return DAS_S_OK;
            }

            DasResult ProcessReadyRequest(
                const ReadyRequestV1& request,
                ReadyAckV1&           response)
            {
                std::lock_guard<std::mutex> lock(mutex_);

                auto it = pending_connections_.find(request.session_id);
                if (it == pending_connections_.end())
                {
                    InitReadyAck(response, ReadyAckV1::STATUS_INVALID_SESSION);
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                if (it->second.state != HandshakeState::WelcomeRecv)
                {
                    InitReadyAck(
                        response,
                        ReadyAckV1::STATUS_SESSION_NOT_READY);
                    return DAS_E_IPC_INVALID_STATE;
                }

                // 握手完成，从待确认列表移除
                pending_connections_.erase(it);

                InitReadyAck(response, ReadyAckV1::STATUS_SUCCESS);
                return DAS_S_OK;
            }

            void ReleaseSessionId(uint16_t session_id)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_connections_.erase(session_id);
            }

        private:
            struct PendingConnection
            {
                uint16_t       child_session_id;
                uint16_t       host_session_id;
                uint32_t       pid;
                std::string    plugin_name;
                HandshakeState state;
            };

            std::mutex mutex_;
            std::unordered_map<uint16_t, PendingConnection>
                     pending_connections_;
            uint16_t next_session_id_{MIN_SESSION_ID};

            uint16_t AllocateSessionId()
            {
                uint16_t start_id = next_session_id_;
                do
                {
                    uint16_t id = next_session_id_++;
                    if (next_session_id_ > MAX_SESSION_ID)
                    {
                        next_session_id_ = MIN_SESSION_ID;
                    }

                    // 检查是否已分配
                    if (pending_connections_.find(id)
                        == pending_connections_.end())
                    {
                        return id;
                    }
                } while (next_session_id_ != start_id);

                return INVALID_SESSION_ID;
            }
        };

        // ============================================================
        // Child 端握手管理器
        // ============================================================

        /**
         * @brief Child 端握手管理器
         *
         * 负责发起握手请求，处理 Host 的响应。
         */
        class ChildHandshakeManager
        {
        public:
            ChildHandshakeManager()
                : state_(HandshakeState::Disconnected), session_id_(0)
            {
            }

            ~ChildHandshakeManager() = default;

            HandshakeState GetState() const { return state_; }

            uint16_t GetSessionId() const { return session_id_; }

            HelloRequestV1 CreateHelloRequest(
                uint32_t    pid,
                const char* plugin_name)
            {
                HelloRequestV1 request;
                InitHelloRequest(request, pid, plugin_name);
                state_ = HandshakeState::HelloSent;
                return request;
            }

            DasResult ProcessWelcomeResponse(const WelcomeResponseV1& response)
            {
                if (state_ != HandshakeState::HelloSent)
                {
                    state_ = HandshakeState::Failed;
                    return DAS_E_IPC_INVALID_STATE;
                }

                if (response.status != WelcomeResponseV1::STATUS_SUCCESS)
                {
                    state_ = HandshakeState::Failed;
                    return DAS_E_IPC_HANDSHAKE_FAILED;
                }

                if (response.session_id == 0)
                {
                    state_ = HandshakeState::Failed;
                    return DAS_E_IPC_HANDSHAKE_FAILED;
                }

                session_id_ = response.session_id;
                state_ = HandshakeState::WelcomeRecv;
                return DAS_S_OK;
            }

            ReadyRequestV1 CreateReadyRequest()
            {
                ReadyRequestV1 request;
                InitReadyRequest(request, session_id_);
                state_ = HandshakeState::ReadySent;
                return request;
            }

            DasResult ProcessReadyAck(const ReadyAckV1& ack)
            {
                if (state_ != HandshakeState::ReadySent)
                {
                    state_ = HandshakeState::Failed;
                    return DAS_E_IPC_INVALID_STATE;
                }

                if (ack.status != ReadyAckV1::STATUS_SUCCESS)
                {
                    state_ = HandshakeState::Failed;
                    return DAS_E_IPC_HANDSHAKE_FAILED;
                }

                state_ = HandshakeState::Connected;
                return DAS_S_OK;
            }

            HandshakeResult GetResult() const
            {
                HandshakeResult result;
                result.state = state_;
                result.session_id =
                    (state_ == HandshakeState::Connected) ? session_id_ : 0;
                result.error_code = (state_ == HandshakeState::Failed)
                                        ? DAS_E_IPC_HANDSHAKE_FAILED
                                        : 0;
                return result;
            }

            void Reset()
            {
                state_ = HandshakeState::Disconnected;
                session_id_ = 0;
            }

        private:
            HandshakeState state_;
            uint16_t       session_id_;
        };

        // ============================================================
        // 心跳辅助函数
        // ============================================================

        HeartbeatV1 CreateHeartbeat()
        {
            HeartbeatV1 hb;
            hb.timestamp_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            return hb;
        }

        GoodbyeV1 CreateGoodbye(GoodbyeReason reason)
        {
            GoodbyeV1 gb;
            InitGoodbye(gb, reason);
            return gb;
        }

    }
}
DAS_NS_END

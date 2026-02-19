#include <cstring>
#include <das/Core/IPC/Host/HandshakeHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace Host
        {

            HandshakeHandler::HandshakeHandler()
                : local_session_id_(0), initialized_(false)
            {
            }

            HandshakeHandler::~HandshakeHandler()
            {
                if (initialized_)
                {
                    Shutdown();
                }
            }

            DasResult HandshakeHandler::Initialize(uint16_t local_session_id)
            {
                if (initialized_)
                {
                    return DAS_S_OK;
                }

                if (!SessionCoordinator::IsValidSessionId(local_session_id))
                {
                    std::string msg = DAS_FMT_NS::format(
                        "HandshakeHandler: Invalid local_session_id: {}",
                        local_session_id);
                    DAS_LOG_ERROR(msg.c_str());
                    return DAS_E_IPC_INVALID_ARGUMENT;
                }

                local_session_id_ = local_session_id;
                initialized_ = true;

                std::string msg = DAS_FMT_NS::format(
                    "HandshakeHandler initialized with session_id: {}",
                    local_session_id);
                DAS_LOG_INFO(msg.c_str());
                return DAS_S_OK;
            }

            DasResult HandshakeHandler::Shutdown()
            {
                if (!initialized_)
                {
                    return DAS_S_OK;
                }

                std::lock_guard<std::mutex> lock(clients_mutex_);

                auto& coordinator = SessionCoordinator::GetInstance();

                for (auto& pair : clients_)
                {
                    coordinator.ReleaseSessionId(pair.first);

                    if (on_client_disconnected_)
                    {
                        on_client_disconnected_(pair.first);
                    }
                }

                clients_.clear();
                initialized_ = false;

                std::string msg =
                    DAS_FMT_NS::format("HandshakeHandler shutdown complete");
                DAS_LOG_INFO(msg.c_str());
                return DAS_S_OK;
            }

            DasResult HandshakeHandler::HandleMessage(
                const IPCMessageHeader& header,
                const uint8_t*          body,
                size_t                  body_size,
                std::vector<uint8_t>&   response_body)
            {
                if (!initialized_)
                {
                    std::string msg =
                        DAS_FMT_NS::format("HandshakeHandler not initialized");
                    DAS_LOG_ERROR(msg.c_str());
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                HandshakeInterfaceId interface_id =
                    static_cast<HandshakeInterfaceId>(header.interface_id);

                switch (interface_id)
                {
                case HandshakeInterfaceId::HandshakeHello:
                {
                    if (body_size < sizeof(HelloRequestV1))
                    {
                        std::string msg = DAS_FMT_NS::format(
                            "HandshakeHandler: HelloRequest body too small: {} < {}",
                            body_size,
                            sizeof(HelloRequestV1));
                        DAS_LOG_ERROR(msg.c_str());
                        return DAS_E_IPC_INVALID_MESSAGE_BODY;
                    }

                    const HelloRequestV1* request =
                        reinterpret_cast<const HelloRequestV1*>(body);
                    return HandleHelloRequest(*request, response_body);
                }

                case HandshakeInterfaceId::HandshakeReady:
                {
                    if (body_size < sizeof(ReadyRequestV1))
                    {
                        std::string msg = DAS_FMT_NS::format(
                            "HandshakeHandler: ReadyRequest body too small: {} < {}",
                            body_size,
                            sizeof(ReadyRequestV1));
                        DAS_LOG_ERROR(msg.c_str());
                        return DAS_E_IPC_INVALID_MESSAGE_BODY;
                    }

                    const ReadyRequestV1* request =
                        reinterpret_cast<const ReadyRequestV1*>(body);
                    return HandleReadyRequest(*request, response_body);
                }

                case HandshakeInterfaceId::Heartbeat:
                {
                    if (body_size < sizeof(HeartbeatV1))
                    {
                        std::string msg = DAS_FMT_NS::format(
                            "HandshakeHandler: Heartbeat body too small: {} < {}",
                            body_size,
                            sizeof(HeartbeatV1));
                        DAS_LOG_ERROR(msg.c_str());
                        return DAS_E_IPC_INVALID_MESSAGE_BODY;
                    }

                    const HeartbeatV1* heartbeat =
                        reinterpret_cast<const HeartbeatV1*>(body);
                    return HandleHeartbeat(*heartbeat);
                }

                case HandshakeInterfaceId::Goodbye:
                {
                    if (body_size < sizeof(GoodbyeV1))
                    {
                        std::string msg = DAS_FMT_NS::format(
                            "HandshakeHandler: Goodbye body too small: {} < {}",
                            body_size,
                            sizeof(GoodbyeV1));
                        DAS_LOG_ERROR(msg.c_str());
                        return DAS_E_IPC_INVALID_MESSAGE_BODY;
                    }

                    const GoodbyeV1* goodbye =
                        reinterpret_cast<const GoodbyeV1*>(body);
                    return HandleGoodbye(*goodbye);
                }

                default:
                    std::string msg = DAS_FMT_NS::format(
                        "HandshakeHandler: Unknown interface_id: {}",
                        header.interface_id);
                    DAS_LOG_ERROR(msg.c_str());
                    return DAS_E_IPC_INVALID_INTERFACE_ID;
                }
            }

            void HandshakeHandler::SetOnClientConnected(
                ClientConnectedCallback callback)
            {
                on_client_connected_ = std::move(callback);
            }

            void HandshakeHandler::SetOnClientDisconnected(
                ClientDisconnectedCallback callback)
            {
                on_client_disconnected_ = std::move(callback);
            }

            bool HandshakeHandler::HasClient(uint16_t session_id) const
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                return clients_.find(session_id) != clients_.end();
            }

            const ConnectedClient* HandshakeHandler::GetClient(
                uint16_t session_id) const
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                auto                        it = clients_.find(session_id);
                if (it != clients_.end())
                {
                    return &it->second;
                }
                return nullptr;
            }

            std::vector<ConnectedClient> HandshakeHandler::GetAllClients() const
            {
                std::lock_guard<std::mutex>  lock(clients_mutex_);
                std::vector<ConnectedClient> result;
                result.reserve(clients_.size());
                for (const auto& pair : clients_)
                {
                    result.push_back(pair.second);
                }
                return result;
            }

            size_t HandshakeHandler::GetClientCount() const
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                return clients_.size();
            }

            bool HandshakeHandler::IsInitialized() const
            {
                return initialized_;
            }

            DasResult HandshakeHandler::HandleHelloRequest(
                const HelloRequestV1& request,
                std::vector<uint8_t>& response_body)
            {
                if (request.protocol_version
                    != HelloRequestV1::CURRENT_PROTOCOL_VERSION)
                {
                    std::string msg = DAS_FMT_NS::format(
                        "HandshakeHandler: Protocol version mismatch: {} != {}",
                        request.protocol_version,
                        HelloRequestV1::CURRENT_PROTOCOL_VERSION);
                    DAS_LOG_ERROR(msg.c_str());

                    WelcomeResponseV1 response;
                    InitWelcomeResponse(
                        response,
                        0,
                        WelcomeResponseV1::STATUS_VERSION_MISMATCH);

                    response_body.resize(sizeof(response));
                    std::memcpy(
                        response_body.data(),
                        &response,
                        sizeof(response));
                    return DAS_E_IPC_INVALID_MESSAGE;
                }

                if (request.plugin_name[0] == '\0')
                {
                    std::string msg = DAS_FMT_NS::format(
                        "HandshakeHandler: Empty plugin name");
                    DAS_LOG_ERROR(msg.c_str());

                    WelcomeResponseV1 response;
                    InitWelcomeResponse(
                        response,
                        0,
                        WelcomeResponseV1::STATUS_INVALID_NAME);

                    response_body.resize(sizeof(response));
                    std::memcpy(
                        response_body.data(),
                        &response,
                        sizeof(response));
                    return DAS_E_IPC_INVALID_MESSAGE;
                }

                auto&    coordinator = SessionCoordinator::GetInstance();
                uint16_t session_id = coordinator.AllocateSessionId();

                if (session_id == 0)
                {
                    std::string msg = DAS_FMT_NS::format(
                        "HandshakeHandler: Failed to allocate session_id");
                    DAS_LOG_ERROR(msg.c_str());

                    WelcomeResponseV1 response;
                    InitWelcomeResponse(
                        response,
                        0,
                        WelcomeResponseV1::STATUS_TOO_MANY_CLIENTS);

                    response_body.resize(sizeof(response));
                    std::memcpy(
                        response_body.data(),
                        &response,
                        sizeof(response));
                    return DAS_E_IPC_SESSION_ALLOC_FAILED;
                }

                ConnectedClient client;
                client.session_id = session_id;
                client.pid = request.pid;
                client.plugin_name = request.plugin_name;
                client.is_ready = false;
                client.last_heartbeat = std::chrono::steady_clock::now();

                {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    clients_[session_id] = client;
                }

                std::string msg = DAS_FMT_NS::format(
                    "HandshakeHandler: Client connected: session_id={}, pid={}, plugin={}",
                    session_id,
                    request.pid,
                    request.plugin_name);
                DAS_LOG_INFO(msg.c_str());

                WelcomeResponseV1 response;
                InitWelcomeResponse(
                    response,
                    session_id,
                    WelcomeResponseV1::STATUS_SUCCESS);

                response_body.resize(sizeof(response));
                std::memcpy(response_body.data(), &response, sizeof(response));

                if (on_client_connected_)
                {
                    on_client_connected_(client);
                }

                return DAS_S_OK;
            }

            DasResult HandshakeHandler::HandleReadyRequest(
                const ReadyRequestV1& request,
                std::vector<uint8_t>& response_body)
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);

                auto it = clients_.find(request.session_id);
                if (it == clients_.end())
                {
                    std::string msg = DAS_FMT_NS::format(
                        "HandshakeHandler: ReadyRequest for unknown session: {}",
                        request.session_id);
                    DAS_LOG_ERROR(msg.c_str());

                    ReadyAckV1 ack;
                    InitReadyAck(ack, ReadyAckV1::STATUS_INVALID_SESSION);

                    response_body.resize(sizeof(ack));
                    std::memcpy(response_body.data(), &ack, sizeof(ack));
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                if (it->second.is_ready)
                {
                    std::string msg = DAS_FMT_NS::format(
                        "HandshakeHandler: Client already ready: session_id={}",
                        request.session_id);
                    DAS_LOG_WARNING(msg.c_str());

                    ReadyAckV1 ack;
                    InitReadyAck(ack, ReadyAckV1::STATUS_SUCCESS);

                    response_body.resize(sizeof(ack));
                    std::memcpy(response_body.data(), &ack, sizeof(ack));
                    return DAS_S_OK;
                }

                it->second.is_ready = true;

                std::string msg = DAS_FMT_NS::format(
                    "HandshakeHandler: Client ready: session_id={}, plugin={}",
                    request.session_id,
                    it->second.plugin_name.c_str());
                DAS_LOG_INFO(msg.c_str());

                ReadyAckV1 ack;
                InitReadyAck(ack, ReadyAckV1::STATUS_SUCCESS);

                response_body.resize(sizeof(ack));
                std::memcpy(response_body.data(), &ack, sizeof(ack));
                return DAS_S_OK;
            }

            DasResult HandshakeHandler::HandleHeartbeat(
                const HeartbeatV1& heartbeat)
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);

                bool found = false;
                for (auto& pair : clients_)
                {
                    if (pair.second.is_ready)
                    {
                        pair.second.last_heartbeat =
                            std::chrono::steady_clock::now();
                        found = true;
                    }
                }

                if (!found)
                {
                    (void)heartbeat;
                }

                return DAS_S_OK;
            }

            DasResult HandshakeHandler::HandleGoodbye(const GoodbyeV1& goodbye)
            {
                uint16_t session_id = 0;

                {
                    std::lock_guard<std::mutex> lock(clients_mutex_);

                    if (!clients_.empty())
                    {
                        auto it = clients_.begin();
                        session_id = it->first;
                        clients_.erase(it);
                    }
                }

                if (session_id != 0)
                {
                    auto& coordinator = SessionCoordinator::GetInstance();
                    coordinator.ReleaseSessionId(session_id);

                    std::string msg = DAS_FMT_NS::format(
                        "HandshakeHandler: Client disconnected: session_id={}, reason={}",
                        session_id,
                        goodbye.reason);
                    DAS_LOG_INFO(msg.c_str());

                    if (on_client_disconnected_)
                    {
                        on_client_disconnected_(session_id);
                    }
                }

                (void)goodbye;
                return DAS_S_OK;
            }

            std::unordered_map<uint16_t, ConnectedClient>::iterator
            HandshakeHandler::FindClientBySessionId(uint16_t session_id)
            {
                return clients_.find(session_id);
            }

        } // namespace Host
    } // namespace IPC
} // namespace Core
DAS_NS_END

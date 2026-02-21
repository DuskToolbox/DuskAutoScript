#ifndef DAS_CORE_IPC_HOST_HANDSHAKE_HANDLER_H
#define DAS_CORE_IPC_HOST_HANDSHAKE_HANDLER_H

#include <chrono>
#include <cstdint>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <das/Utils/fmt.h>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace Host
        {
            /**
             * @brief 已连接客户端信息
             *
             * 存储已通过握手协议连接的客户端会话信息。
             */
            struct ConnectedClient
            {
                uint16_t    session_id;  ///< 分配的 session_id
                uint32_t    pid;         ///< 客户端进程 ID
                std::string plugin_name; ///< 插件名称
                bool        is_ready;    ///< 是否已完成 Ready 握手

                /**
                 * @brief 最后心跳时间戳
                 */
                std::chrono::steady_clock::time_point last_heartbeat;
            };

            /**
             * @brief Host 进程握手处理器
             *
             * 处理来自 Child/Client 进程的握手请求：
             * - HelloRequestV1 → WelcomeResponseV1
             * - ReadyRequestV1 → ReadyAckV1
             * - HeartbeatV1
             * - GoodbyeV1
             *
             * 该类可以作为 IpcRunLoop::SetRequestHandler 的处理器使用。
             *
             * @note 线程安全：所有公共方法都是线程安全的。
             *
             * ## 使用示例
             *
             * @code{.cpp}
             * using namespace Das::Core::IPC;
             *
             * // 创建握手处理器
             * Host::HandshakeHandler handler;
             * handler.Initialize(1);  // Host 的 session_id
             *
             * // 设置回调
             * handler.SetOnClientConnected([](const Host::ConnectedClient&
             * client) {
             *     std::string msg = DAS_FMT_NS::format(
             *         "Client connected: session_id={}, plugin={}",
             *         client.session_id, client.plugin_name.c_str());
             *     DAS_LOG_INFO(msg.c_str()); });
             *
             * // 作为 IpcRunLoop 的请求处理器
             * run_loop.SetRequestHandler([&handler](
             *     const IPCMessageHeader& header,
             *     const uint8_t* body,
             *     size_t body_size) -> DasResult
             * {
             *     std::vector<uint8_t> response;
             *     return handler.HandleMessage(header, body, body_size,
             * response);
             * });
             * @endcode
             */
            class DAS_API HandshakeHandler
            {
            public:
                using ClientConnectedCallback =
                    std::function<void(const ConnectedClient&)>;
                using ClientDisconnectedCallback =
                    std::function<void(uint16_t session_id)>;

                /**
                 * @brief 构造函数
                 */
                HandshakeHandler();

                /**
                 * @brief 析构函数
                 */
                ~HandshakeHandler();

                // 禁用拷贝
                HandshakeHandler(const HandshakeHandler&) = delete;
                HandshakeHandler& operator=(const HandshakeHandler&) = delete;

                /**
                 * @brief 初始化处理器
                 *
                 * @param local_session_id 本 Host 进程的 session_id
                 * @return DasResult 成功返回 DAS_S_OK
                 */
                DasResult Initialize(uint16_t local_session_id);

                /**
                 * @brief 关闭处理器
                 *
                 * 释放所有客户端连接，释放分配的 session_id。
                 *
                 * @return DasResult 成功返回 DAS_S_OK
                 */
                DasResult Shutdown();

                /**
                 * @brief 处理握手消息
                 *
                 * 可作为 IpcRunLoop::SetRequestHandler 的参数使用。
                 * 根据 interface_id 分发到对应的处理方法。
                 *
                 * @param header 消息头
                 * @param body 消息体
                 * @param body_size 消息体大小
                 * @param response_body 输出响应体
                 * @return DasResult 成功返回 DAS_S_OK
                 */
                DasResult HandleMessage(
                    const IPCMessageHeader& header,
                    const uint8_t*          body,
                    size_t                  body_size,
                    std::vector<uint8_t>&   response_body);

                /**
                 * @brief 设置客户端连接回调
                 *
                 * 当客户端完成 Hello 握手后触发。
                 *
                 * @param callback 回调函数
                 */
                void SetOnClientConnected(ClientConnectedCallback callback);

                /**
                 * @brief 设置客户端断开回调
                 *
                 * 当客户端发送 Goodbye 或超时时触发。
                 *
                 * @param callback 回调函数
                 */
                void SetOnClientDisconnected(
                    ClientDisconnectedCallback callback);

                /**
                 * @brief 检查是否存在指定客户端
                 *
                 * @param session_id 客户端 session_id
                 * @return true 如果客户端存在
                 */
                bool HasClient(uint16_t session_id) const;

                /**
                 * @brief 获取客户端信息
                 *
                 * @param session_id 客户端 session_id
                 * @return 客户端信息指针，如果不存在返回 nullptr
                 */
                const ConnectedClient* GetClient(uint16_t session_id) const;

                /**
                 * @brief 获取所有已连接的客户端
                 *
                 * @return 客户端列表
                 */
                std::vector<ConnectedClient> GetAllClients() const;

                /**
                 * @brief 获取已连接客户端数量
                 *
                 * @return 客户端数量
                 */
                size_t GetClientCount() const;

                /**
                 * @brief 检查处理器是否已初始化
                 *
                 * @return true 如果已初始化
                 */
                bool IsInitialized() const;

            private:
                /**
                 * @brief 处理 HelloRequestV1
                 *
                 * - 验证协议版本
                 * - 分配 session_id
                 * - 创建 WelcomeResponseV1
                 *
                 * @param request 请求
                 * @param response_body 输出响应体
                 * @return DasResult
                 */
                DasResult HandleHelloRequest(
                    const HelloRequestV1& request,
                    std::vector<uint8_t>& response_body);

                /**
                 * @brief 处理 ReadyRequestV1
                 *
                 * - 验证 session_id
                 * - 标记客户端为 ready
                 * - 创建 ReadyAckV1
                 *
                 * @param request 请求
                 * @param response_body 输出响应体
                 * @return DasResult
                 */
                DasResult HandleReadyRequest(
                    const ReadyRequestV1& request,
                    std::vector<uint8_t>& response_body);

                /**
                 * @brief 处理 HeartbeatV1
                 *
                 * - 更新客户端心跳时间戳
                 *
                 * @param heartbeat 心跳消息
                 * @return DasResult
                 */
                DasResult HandleHeartbeat(const HeartbeatV1& heartbeat);

                /**
                 * @brief 处理 GoodbyeV1
                 *
                 * - 移除客户端
                 * - 释放 session_id
                 * - 触发断开回调
                 *
                 * @param goodbye 断开消息
                 * @return DasResult
                 */
                DasResult HandleGoodbye(const GoodbyeV1& goodbye);

                /**
                 * @brief 根据心跳时间戳查找客户端
                 *
                 * @param session_id 客户端 session_id
                 * @return 客户端迭代器
                 */
                std::unordered_map<uint16_t, ConnectedClient>::iterator
                FindClientBySessionId(uint16_t session_id);

                // 成员变量
                uint16_t local_session_id_; ///< 本 Host 进程的 session_id
                bool     initialized_;      ///< 是否已初始化

                mutable std::mutex clients_mutex_; ///< 客户端列表锁
                std::unordered_map<uint16_t, ConnectedClient>
                    clients_; ///< 已连接客户端

                ClientConnectedCallback
                    on_client_connected_; ///< 客户端连接回调
                ClientDisconnectedCallback
                    on_client_disconnected_; ///< 客户端断开回调
            };

        } // namespace Host
    } // namespace IPC
} // namespace Core
DAS_NS_END

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // DAS_CORE_IPC_HOST_HANDSHAKE_HANDLER_H

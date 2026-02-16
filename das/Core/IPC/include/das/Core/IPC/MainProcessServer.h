#ifndef DAS_CORE_IPC_MAIN_PROCESS_SERVER_H
#define DAS_CORE_IPC_MAIN_PROCESS_SERVER_H

#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/IDasBase.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        class IpcRunLoop;
        class ConnectionManager;

        /**
         * @brief 会话连接信息
         */
        struct HostSessionInfo
        {
            uint16_t session_id;      ///< Host进程的会话ID
            bool     is_connected;    ///< 连接状态
            uint64_t connect_time_ms; ///< 连接时间戳（毫秒）
            uint64_t last_active_ms;  ///< 最后活动时间戳（毫秒）
        };

        /**
         * @brief 消息分发回调类型
         *
         * @param header 消息头
         * @param body 消息体数据
         * @param body_size 消息体大小
         * @param response_body 输出响应体
         * @return DasResult 处理结果
         */
        using MessageDispatchHandler = std::function<DasResult(
            const IPCMessageHeader& header,
            const uint8_t*          body,
            size_t                  body_size,
            std::vector<uint8_t>&   response_body)>;

        /**
         * @brief 主进程 IPC 服务端
         *
         * 负责监听 Host 进程连接、维护 RemoteObjectRegistry、分发 IPC 消息。
         *
         * 典型架构:
         *   主进程 (MainProcessServer)
         *       │
         *       ├── OnHostConnected() ──→ 注册会话
         *       │
         *       ├── OnRemoteObjectRegistered() ──→ 更新 RemoteObjectRegistry
         *       │
         *       └── DispatchMessage() ──→ 分发到目标对象
         */
        class MainProcessServer
        {
        public:
            /**
             * @brief 获取 MainProcessServer 单例实例
             */
            static MainProcessServer& GetInstance();

            /**
             * @brief 初始化服务端
             *
             * @return DasResult 初始化结果
             */
            DasResult Initialize();

            /**
             * @brief 关闭服务端
             *
             * @return DasResult 关闭结果
             */
            DasResult Shutdown();

            /**
             * @brief 启动消息循环
             *
             * @return DasResult 启动结果
             */
            DasResult Start();

            /**
             * @brief 停止消息循环
             *
             * @return DasResult 停止结果
             */
            DasResult Stop();

            /**
             * @brief 检查服务端是否正在运行
             *
             * @return true 如果正在运行
             */
            bool IsRunning() const;

            // ====== 会话管理 ======

            /**
             * @brief 处理 Host 进程连接事件
             *
             * @param session_id Host 进程的会话ID
             * @return DasResult 处理结果
             */
            DasResult OnHostConnected(uint16_t session_id);

            /**
             * @brief 处理 Host 进程断开事件
             *
             * @param session_id Host 进程的会话ID
             * @return DasResult 处理结果
             */
            DasResult OnHostDisconnected(uint16_t session_id);

            /**
             * @brief 检查会话是否已连接
             *
             * @param session_id 会话ID
             * @return true 如果已连接
             */
            bool IsSessionConnected(uint16_t session_id) const;

            /**
             * @brief 获取所有已连接的会话ID
             *
             * @return 已连接的会话ID列表
             */
            std::vector<uint16_t> GetConnectedSessions() const;

            /**
             * @brief 获取会话信息
             *
             * @param session_id 会话ID
             * @param out_info 输出会话信息
             * @return DasResult 查找结果
             */
            DasResult GetSessionInfo(
                uint16_t         session_id,
                HostSessionInfo& out_info) const;

            // ====== 远程对象管理 ======

            /**
             * @brief 处理远程对象注册事件
             *
             * @param object_id 对象ID
             * @param iid 接口ID
             * @param session_id 会话ID
             * @param name 对象名称
             * @param version 接口版本
             * @return DasResult 处理结果
             */
            DasResult OnRemoteObjectRegistered(
                const ObjectId&    object_id,
                const DasGuid&     iid,
                uint16_t           session_id,
                const std::string& name,
                uint16_t           version = 1);

            /**
             * @brief 处理远程对象注销事件
             *
             * @param object_id 对象ID
             * @return DasResult 处理结果
             */
            DasResult OnRemoteObjectUnregistered(const ObjectId& object_id);

            /**
             * @brief 获取所有远程对象
             *
             * @param out_objects 输出对象列表
             * @return DasResult 查询结果
             */
            DasResult GetRemoteObjects(
                std::vector<RemoteObjectInfo>& out_objects) const;

            /**
             * @brief 获取指定远程对象信息
             *
             * @param object_id 对象ID
             * @param out_info 输出对象信息
             * @return DasResult 查询结果
             */
            DasResult GetRemoteObjectInfo(
                const ObjectId&   object_id,
                RemoteObjectInfo& out_info) const;

            /**
             * @brief 通过名称查找远程对象
             *
             * @param name 对象名称
             * @param out_info 输出对象信息
             * @return DasResult 查询结果
             */
            DasResult LookupRemoteObjectByName(
                const std::string& name,
                RemoteObjectInfo&  out_info) const;

            /**
             * @brief 通过接口类型查找远程对象
             *
             * @param iid 接口ID
             * @param out_info 输出对象信息
             * @return DasResult 查询结果
             */
            DasResult LookupRemoteObjectByInterface(
                const DasGuid&    iid,
                RemoteObjectInfo& out_info) const;

            // ====== 消息分发 ======

            /**
             * @brief 分发消息到目标对象
             *
             * @param header 消息头
             * @param body 消息体数据
             * @param body_size 消息体大小
             * @param response_body 输出响应体
             * @return DasResult 处理结果
             */
            DasResult DispatchMessage(
                const IPCMessageHeader& header,
                const uint8_t*          body,
                size_t                  body_size,
                std::vector<uint8_t>&   response_body);

            /**
             * @brief 设置消息分发处理器
             *
             * @param handler 处理器回调
             */
            void SetMessageDispatchHandler(MessageDispatchHandler handler);

            // ====== 事件回调 ======

            /**
             * @brief 会话连接事件回调类型
             */
            using SessionEventCallback =
                std::function<void(uint16_t session_id)>;

            /**
             * @brief 设置会话连接回调
             *
             * @param callback 回调函数
             */
            void SetOnSessionConnectedCallback(SessionEventCallback callback);

            /**
             * @brief 设置会话断开回调
             *
             * @param callback 回调函数
             */
            void SetOnSessionDisconnectedCallback(
                SessionEventCallback callback);

            /**
             * @brief 对象注册事件回调类型
             */
            using ObjectEventCallback =
                std::function<void(const RemoteObjectInfo& info)>;

            /**
             * @brief 设置对象注册回调
             *
             * @param callback 回调函数
             */
            void SetOnObjectRegisteredCallback(ObjectEventCallback callback);

            /**
             * @brief 设置对象注销回调
             *
             * @param callback 回调函数
             */
            void SetOnObjectUnregisteredCallback(ObjectEventCallback callback);

            // 禁止拷贝和赋值
            MainProcessServer(const MainProcessServer&) = delete;
            MainProcessServer& operator=(const MainProcessServer&) = delete;

        private:
            // 私有构造函数（单例模式）
            MainProcessServer();
            ~MainProcessServer();

            /**
             * @brief 获取当前时间戳（毫秒）
             */
            static uint64_t GetCurrentTimeMs();

            /**
             * @brief 验证会话ID是否有效
             */
            bool ValidateSessionId(uint16_t session_id) const;

            /**
             * @brief 验证消息目标对象是否存在
             */
            DasResult ValidateTargetObject(
                const IPCMessageHeader& header) const;

            // 成员变量
            mutable std::mutex                            sessions_mutex_;
            std::unordered_map<uint16_t, HostSessionInfo> sessions_;

            MessageDispatchHandler dispatch_handler_;
            SessionEventCallback   on_session_connected_;
            SessionEventCallback   on_session_disconnected_;
            ObjectEventCallback    on_object_registered_;
            ObjectEventCallback    on_object_unregistered_;

            std::atomic<bool> is_running_{false};
            std::atomic<bool> is_initialized_{false};
        };
    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_MAIN_PROCESS_SERVER_H

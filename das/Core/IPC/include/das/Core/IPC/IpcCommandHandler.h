#ifndef DAS_CORE_IPC_IPC_COMMAND_HANDLER_H
#define DAS_CORE_IPC_IPC_COMMAND_HANDLER_H

#include <cstdint>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>
#include <functional>
#include <span>
#include <string>
#include <vector>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        /**
         * @brief IPC 命令类型枚举
         *
         * 定义主进程和 Host 进程之间的控制命令类型
         */
        enum class IpcCommandType : uint8_t
        {
            // 对象管理命令
            REGISTER_OBJECT = 1,      // 注册远程对象
            UNREGISTER_OBJECT = 2,    // 注销远程对象
            LOOKUP_OBJECT = 3,        // 通过 ObjectId 查找对象
            LOOKUP_BY_NAME = 4,       // 通过名称查找对象
            LOOKUP_BY_INTERFACE = 5,  // 通过接口类型查找对象
            LIST_OBJECTS = 6,         // 列出所有对象
            LIST_SESSION_OBJECTS = 7, // 列出指定会话的对象
            CLEAR_SESSION = 8,        // 清除指定会话的所有对象
            LOAD_PLUGIN = 9,          // 加载插件

            // 心跳和状态命令
            PING = 10, // 心跳请求
            PONG = 11, // 心跳响应

            // 查询命令
            GET_OBJECT_COUNT = 20, // 获取对象数量

            // 保留
            UNKNOWN = 255
        };

        /**
         * @brief IPC 命令响应结构
         */
        struct IpcCommandResponse
        {
            DasResult            error_code;    // 错误码
            std::vector<uint8_t> response_data; // 响应数据
        };

        /**
         * @brief IPC 命令处理器
         *
         * 处理主进程和 Host 进程之间的控制命令，
         * 与 RemoteObjectRegistry 集成进行对象管理
         */
        class IpcCommandHandler
        {
        public:
            // 命令处理函数类型
            using CommandHandler = std::function<DasResult(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response)>;

            IpcCommandHandler();
            ~IpcCommandHandler() = default;

            // 禁止拷贝
            IpcCommandHandler(const IpcCommandHandler&) = delete;
            IpcCommandHandler& operator=(const IpcCommandHandler&) = delete;

            /**
             * @brief 处理 IPC 命令
             *
             * @param header 消息头
             * @param payload 命令负载数据
             * @param response [out] 响应结构
             * @return DasResult 处理结果
             */
            DasResult HandleCommand(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            /**
             * @brief 注册自定义命令处理器
             *
             * @param command_type 命令类型
             * @param handler 处理函数
             */
            void RegisterHandler(
                IpcCommandType command_type,
                CommandHandler handler);

            /**
             * @brief 设置当前会话ID
             *
             * @param session_id 会话ID
             */
            void SetSessionId(uint16_t session_id);

            /**
             * @brief 获取当前会话ID
             *
             * @return uint16_t 会话ID
             */
            uint16_t GetSessionId() const;

        private:
            // 内置命令处理器
            DasResult OnRegisterObject(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            DasResult OnUnregisterObject(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            DasResult OnLookupObject(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            DasResult OnLookupByName(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            DasResult OnLookupByInterface(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            DasResult OnListObjects(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            DasResult OnListSessionObjects(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            DasResult OnClearSession(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            DasResult OnPing(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            DasResult OnGetObjectCount(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            DasResult OnLoadPlugin(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response);

            // 从 interface_id 字段提取命令类型
            static IpcCommandType ExtractCommandType(
                const IPCMessageHeader& header);

            uint16_t session_id_;
        };

        // 命令 payload 序列化辅助结构

        /**
         * @brief 注册对象请求 payload
         */
        struct RegisterObjectPayload
        {
            ObjectId object_id;  // 对象ID
            DasGuid  iid;        // 接口ID
            uint16_t session_id; // 会话ID
            uint16_t version;    // 版本
            uint16_t name_len;   // 名称长度
            // char name[name_len]  // 对象名称（UTF-8）
        };

        /**
         * @brief 注销对象请求 payload
         */
        struct UnregisterObjectPayload
        {
            ObjectId object_id; // 要注销的对象ID
        };

        /**
         * @brief 查找对象请求 payload
         */
        struct LookupObjectPayload
        {
            ObjectId object_id; // 要查找的对象ID
        };

        /**
         * @brief 按名称查找请求 payload
         */
        struct LookupByNamePayload
        {
            uint16_t name_len; // 名称长度
            // char name[name_len]
        };

        /**
         * @brief 按接口查找请求 payload
         */
        struct LookupByInterfacePayload
        {
            DasGuid iid; // 接口ID
        };

        /**
         * @brief 列出会话对象请求 payload
         */
        struct ListSessionObjectsPayload
        {
            uint16_t session_id; // 会话ID
        };

        /**
         * @brief 清除会话请求 payload
         */
        struct ClearSessionPayload
        {
            uint16_t session_id; // 要清除的会话ID
        };

        /**
         * @brief 对象信息响应 payload
         */
        struct ObjectInfoResponsePayload
        {
            ObjectId object_id;  // 对象ID
            DasGuid  iid;        // 接口ID
            uint16_t session_id; // 会话ID
            uint16_t version;    // 版本
            uint16_t name_len;   // 名称长度
            // char name[name_len]
        };

        /**
         * @brief 心跳响应 payload
         */
        struct PongPayload
        {
            uint64_t timestamp; // 服务器时间戳
        };

        /**
         * @brief 对象数量响应 payload
         */
        struct ObjectCountResponsePayload
        {
            uint64_t count; // 对象数量
        };

        /**
         * @brief 加载插件请求 payload
         */
        struct LoadPluginPayload
        {
            uint16_t plugin_path_len; // 路径长度
            // char plugin_path[plugin_path_len]  // 插件路径 (UTF-8)
        };

        /**
         * @brief 加载插件响应 payload
         */
        struct LoadPluginResponsePayload
        {
            ObjectId object_id;  // 创建的插件对象ID
            DasGuid  iid;        // 接口ID (IDasBase)
            uint16_t session_id; // 会话ID
            uint16_t version;    // 版本
        };

    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_IPC_COMMAND_HANDLER_H

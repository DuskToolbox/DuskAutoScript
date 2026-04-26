#ifndef DAS_CORE_IPC_IPC_COMMAND_HANDLER_H
#define DAS_CORE_IPC_IPC_COMMAND_HANDLER_H

#include <cstdint>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief IPC 命令类型枚举
 *
 * 定义主进程和 Host 进程之间的控制命令类型
 */
enum class IpcCommandType : uint8_t
{
    // 对象管理命令
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

    // 接口查询 (120-129)
    QUERY_INTERFACE = 120, // 远程 QueryInterface 请求

    // 远程引用计数 (130-139)
    REMOTE_ADD_REF = 130, // 远程端增加引用
    REMOTE_RELEASE = 132, // 远程端释放引用

    // 共享内存管理 (140-149)
    RELEASE_SHM_BLOCK = 140, // 释放共享内存块（fire-and-forget EVENT）

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
class IpcCommandHandler : public IMessageHandler
{
public:
    // 命令处理函数类型
    using CommandHandler = std::function<DasResult(
        const ValidatedIPCMessageHeader& header,
        std::span<const uint8_t>         payload,
        IpcCommandResponse&              response)>;

    explicit IpcCommandHandler(RemoteObjectRegistry& registry);
    ~IpcCommandHandler() = default;

    /**
     * @brief 工厂函数：创建 IpcCommandHandler 实例
     *
     * @param registry 远程对象注册表引用（per-IpcContext 依赖）
     * @return DasPtr<IpcCommandHandler> 新创建的实例
     */
    static DasPtr<IpcCommandHandler> Create(RemoteObjectRegistry& registry)
    {
        return DasPtr<IpcCommandHandler>(new IpcCommandHandler(registry));
    }

    DAS_UTILS_IDASBASE_AUTO_IMPL(IpcCommandHandler)

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
        const ValidatedIPCMessageHeader& header,
        std::span<const uint8_t>         payload,
        IpcCommandResponse&              response);

    /**
     * @brief 注册自定义命令处理器
     *
     * @param command_type 命令类型
     * @param handler 处理函数
     */
    void RegisterHandler(IpcCommandType command_type, CommandHandler handler);

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

    [[nodiscard]]
    uint32_t GetInterfaceId() const noexcept override
    {
        // IpcCommandHandler 不是基于 interface_id 路由的处理器
        // 它通过 IpcContext 直接访问，不通过 IpcRunLoop::GetHandler() 查找
        return 0;
    }

    DasResult HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        StubContext&                     ctx) override;

private:
    // 内置命令处理器
    DasResult OnLookupObject(
        const ValidatedIPCMessageHeader& header,
        std::span<const uint8_t>         payload,
        IpcCommandResponse&              response);

    DasResult OnLookupByName(
        const ValidatedIPCMessageHeader& header,
        std::span<const uint8_t>         payload,
        IpcCommandResponse&              response);

    DasResult OnLookupByInterface(
        const ValidatedIPCMessageHeader& header,
        std::span<const uint8_t>         payload,
        IpcCommandResponse&              response);

    DasResult OnListObjects(
        const ValidatedIPCMessageHeader& header,
        std::span<const uint8_t>         payload,
        IpcCommandResponse&              response);

    DasResult OnListSessionObjects(
        const ValidatedIPCMessageHeader& header,
        std::span<const uint8_t>         payload,
        IpcCommandResponse&              response);

    DasResult OnClearSession(
        const ValidatedIPCMessageHeader& header,
        std::span<const uint8_t>         payload,
        IpcCommandResponse&              response);

    DasResult OnPing(
        const ValidatedIPCMessageHeader& header,
        std::span<const uint8_t>         payload,
        IpcCommandResponse&              response);

    DasResult OnGetObjectCount(
        const ValidatedIPCMessageHeader& header,
        std::span<const uint8_t>         payload,
        IpcCommandResponse&              response);

    DasResult OnLoadPlugin(
        const ValidatedIPCMessageHeader& header,
        std::span<const uint8_t>         payload,
        IpcCommandResponse&              response);

    // 从 interface_id 字段提取命令类型
    static IpcCommandType ExtractCommandType(
        const ValidatedIPCMessageHeader& header);

    uint16_t session_id_;

    /// 远程对象注册表引用（构造时注入，per-IpcContext 依赖）
    RemoteObjectRegistry& registry_;

    std::unordered_map<IpcCommandType, CommandHandler> custom_handlers_;
};

// 命令 payload 序列化辅助结构

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

/**
 * @brief 释放共享内存块请求 payload
 */
struct ReleaseShmBlockPayload
{
    uint64_t shm_handle;        // 共享内存块句柄
    uint16_t source_session_id; // 源 session（用于查找对应的 shm_pool）
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_COMMAND_HANDLER_H

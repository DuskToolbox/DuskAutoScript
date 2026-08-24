#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HandshakeSerialization.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteRefHandler.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncCallback.h>
#include <das/IDasBase.h>

#include <span>
#include <utility>

DAS_CORE_IPC_NS_BEGIN

DasPtr<RemoteRefHandler> RemoteRefHandler::Create(
    DistributedObjectManager& object_manager,
    IpcRunLoop&               run_loop)
{
    return DasPtr<RemoteRefHandler>(
        new RemoteRefHandler(object_manager, run_loop));
}

RemoteRefHandler::RemoteRefHandler(
    DistributedObjectManager& object_manager,
    IpcRunLoop&               run_loop)
    : object_manager_(object_manager), run_loop_(run_loop)
{
}

RemoteRefHandler::~RemoteRefHandler() = default;

boost::asio::awaitable<DasResult> RemoteRefHandler::HandleMessage(
    const ValidatedIPCMessageHeader& header,
    const std::vector<uint8_t>&      body,
    IpcResponseSender&               sender,
    ControlPlaneContext&             context)
{
    (void)sender;
    (void)context;

    // 本 handler 只承接 fire-and-forget EVENT；其他消息类型留给
    // 各自的注册路径，不做任何记账
    if (header.GetMessageType() != MessageType::Event)
    {
        co_return DAS_S_OK;
    }

    const auto command_type =
        static_cast<IpcCommandType>(header.GetInterfaceId());
    if (command_type != IpcCommandType::RemoteAddRef
        && command_type != IpcCommandType::RemoteRelease)
    {
        co_return DAS_S_OK;
    }

    const std::span<const uint8_t> payload(body);
    if (payload.size() < sizeof(ObjectId))
    {
        DAS_CORE_LOG_WARN(
            "RemoteRefHandler: ObjectId body truncated, dropped: "
            "interface_id = 0x{:08X}, body_size = {}",
            header.GetInterfaceId(),
            payload.size());
        co_return DAS_S_OK;
    }

    size_t   offset = 0;
    ObjectId object_id;
    if (!DeserializeValue(payload, offset, object_id))
    {
        DAS_CORE_LOG_WARN(
            "RemoteRefHandler: ObjectId body malformed, dropped: "
            "interface_id = 0x{:08X}, body_size = {}",
            header.GetInterfaceId(),
            payload.size());
        co_return DAS_S_OK;
    }

    // holder 身份取自消息头 source session：伪造 RELEASE 只能吃掉自己的
    // holder 配额，碰不到 local pin（T-104.2-02）
    const uint16_t holder_session_id = header.GetSourceSessionId();

    if (command_type == IpcCommandType::RemoteAddRef)
    {
        const DasResult add_result =
            object_manager_.AddRemoteHolder(object_id, holder_session_id);
        if (DAS::IsFailed(add_result))
        {
            DAS_CORE_LOG_WARN(
                "RemoteRefHandler: RemoteAddRef for unknown object ignored: "
                "object_id = (session={}, generation={}, local={}), "
                "source_session = {}, result = {}",
                object_id.session_id,
                object_id.generation,
                object_id.local_id,
                holder_session_id,
                add_result);
        }
        co_return DAS_S_OK;
    }

    DAS::DasPtr<IDasBase> object_to_release;
    const DasResult       release_result = object_manager_.ReleaseRemoteHolder(
        object_id,
        holder_session_id,
        object_to_release);
    if (DAS::IsFailed(release_result))
    {
        DAS_CORE_LOG_WARN(
            "RemoteRefHandler: RemoteRelease for unknown object ignored: "
            "object_id = (session={}, generation={}, local={}), "
            "source_session = {}, result = {}",
            object_id.session_id,
            object_id.generation,
            object_id.local_id,
            holder_session_id,
            release_result);
        co_return DAS_S_OK;
    }

    if (object_to_release)
    {
        // 条目被移除：对象本体唯一一次移出 DOM，final Release 经
        // MakeAsyncCallback move 捕获 + PostToBusinessThread 桥接——
        // DasPtr 在 BusinessThread 上析构即真实对象 Release 留在 BT
        // （线程模型不变量：真实对象析构不落在 IO 线程）。push 失败
        // 由 PostToBusinessThread 既有 Attach 防御释放，停机期无害。
        static_cast<void>(run_loop_.PostToBusinessThread(
            MakeAsyncCallback(
                [released = std::move(object_to_release)]() mutable {})
                .Get()));
    }

    co_return DAS_S_OK;
}

DAS_CORE_IPC_NS_END

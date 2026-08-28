#include <cstdint>
#include <cstring>
#include <das/Core/IPC/IpcFrameReceiveLoop.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <utility>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

std::variant<DasResult, AsyncIpcMessage> FinalizeReceivedFrame(
    SharedMemoryPool* shared_memory_pool,
    AsyncIpcMessage&& frame)
{
    auto&      header = frame.first;
    auto&      body_buffer = frame.second;
    const auto body_size = header.Raw().body_size;

    DAS_LOG_INFO(
        DAS_FMT_NS::format(
            "ReceiveFrameLoop: received header: msg_type={}, "
            "interface_id={}, call_id={}, body_size={}, header_flags={}",
            static_cast<int>(header.Raw().message_type),
            header.Raw().interface_id,
            header.Raw().call_id,
            body_size,
            static_cast<int>(header.Raw().header_flags))
            .c_str());

    if ((header.Raw().header_flags & HeaderFlags::LARGE_MESSAGE_VIA_SHM) == 0)
    {
        return std::move(frame);
    }

    // 捎带协议防御（D-12 互斥）：SHM 重建会静默清零 reserved——bit1
    // 置位的 SHM 帧必为非法组合（合法发送端拼装规则永不产生），整帧丢弃
    if (header.GetFlags() & MessageFlags::RESERVED_AS_SIZE)
    {
        DAS_CORE_LOG_WARN(
            "SHM frame with RESERVED_AS_SIZE flag rejected: flags = {}",
            header.GetFlags());
        return DAS_E_IPC_INVALID_MESSAGE;
    }

    if (shared_memory_pool == nullptr)
    {
        DAS_LOG_ERROR("Large message received but shared memory pool not set");
        return DAS_E_IPC_INVALID_MESSAGE;
    }

    if (body_buffer.size() < sizeof(uint64_t))
    {
        DAS_LOG_ERROR("Large message handle missing");
        return DAS_E_IPC_INVALID_MESSAGE;
    }

    uint64_t handle = 0;
    std::memcpy(&handle, body_buffer.data(), sizeof(uint64_t));

    const ShmBlockGuard guard =
        ShmBlockGuard::Attach(shared_memory_pool, handle, 0);
    if (!guard)
    {
        DAS_LOG_ERROR(
            DAS_FMT_NS::format(
                "Failed to attach shared memory block for handle = {}",
                handle)
                .c_str());
        return DAS_E_IPC_INVALID_MESSAGE;
    }

    const uint64_t       real_size = *static_cast<uint64_t*>(guard.Data());
    std::vector<uint8_t> large_body(real_size);
    std::memcpy(
        large_body.data(),
        static_cast<uint8_t*>(guard.Data()) + 8,
        real_size);
    // guard 出作用域析构释放

    // 重组返回的 header 要恢复真实 body_size，清掉 LARGE_MESSAGE_VIA_SHM 位
    auto orig = header.Raw();
    auto restored_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(static_cast<MessageType>(orig.message_type))
            .SetHeaderFlags(
                orig.header_flags & ~HeaderFlags::LARGE_MESSAGE_VIA_SHM)
            .SetCallId(orig.call_id)
            .SetSourceSessionId(orig.source_session_id)
            .SetTargetSessionId(orig.target_session_id)
            .SetFlags(orig.flags)
            .SetInterfaceId(orig.interface_id)
            .SetErrorCode(orig.error_code)
            .SetBodySize(static_cast<uint32_t>(real_size))
            .Build();

    return AsyncIpcMessage{restored_header, std::move(large_body)};
}

DAS_CORE_IPC_NS_END

#ifndef DAS_CORE_IPC_FRAME_RECEIVE_LOOP_H
#define DAS_CORE_IPC_FRAME_RECEIVE_LOOP_H

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstddef>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcFrameAssembler.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <utility>
#include <variant>
#include <vector>

DAS_CORE_IPC_NS_BEGIN

class SharedMemoryPool;

/// @brief 收帧尾段（单份实现，两平台传输共用）
///
/// 对重组出的完整帧做语义收尾：SHM 大帧防御（D-12 互斥）、块挂载
/// 重组、header 恢复真实 body_size；非 SHM 帧原样透传。与旧读路径
/// 行为逐位同构（含逐帧 INFO 日志）。
///
/// @param shared_memory_pool 连接的 SHM 池（SHM 帧必需，可空）
/// @param frame 重组器产出的完整帧
/// @return 成功返回最终消息（SHM 帧已重组恢复）；失败返回错误码
std::variant<DasResult, AsyncIpcMessage> FinalizeReceivedFrame(
    SharedMemoryPool* shared_memory_pool,
    AsyncIpcMessage&& frame);

/// @brief 单次读帧接收循环（模板：read 句柄类型两平台不同，逻辑一份）
///
/// - 重组器里已缓冲的完整帧优先取（合写批次残留帧零系统调用）
/// - 否则 read_some 收多少吃多少，撕裂部分留重组器等下一批
/// - 取到帧经 FinalizeReceivedFrame 收尾后返回
///
/// 旧路径每帧每方向付两次 overlapped 读（header/body 各一次，各带
/// 一次 IOCP 完成分发）；本循环热路径一次读一口收全帧（发送端单次
/// WriteFile 落帧的前提下），批合写帧仅付一次读。
///
/// @param read_pipe 平台读句柄（windows::stream_handle /
///                  local::stream_protocol::socket，支持 async_read_some）
/// @param frame_assembler 连接级重组器（残留字节/帧跨调用保留）
/// @param read_stage 复用的接收 staging 缓冲（尺寸不足时自动扩）
/// @param shared_memory_pool SHM 池（透传 FinalizeReceivedFrame）
template <typename ReadPipe>
boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
ReceiveFrameLoop(
    ReadPipe&             read_pipe,
    IpcFrameAssembler&    frame_assembler,
    std::vector<uint8_t>& read_stage,
    SharedMemoryPool*     shared_memory_pool)
{
    // staging 对齐：最大内联帧 + 余量，read_some 尽量一口吃掉合写批次
    const std::size_t stage_need =
        frame_assembler.GetMaxBodySize() + sizeof(IPCMessageHeader) + 4096;
    if (read_stage.size() < stage_need)
    {
        read_stage.resize(stage_need);
    }

    for (;;)
    {
        if (frame_assembler.HasFailed())
        {
            DAS_LOG_ERROR(frame_assembler.FailureDescription().c_str());
            co_return DAS_E_IPC_INVALID_MESSAGE;
        }

        AsyncIpcMessage frame;
        if (frame_assembler.TryTakeFrame(frame))
        {
            co_return FinalizeReceivedFrame(
                shared_memory_pool,
                std::move(frame));
        }

        const std::size_t received = co_await read_pipe.async_read_some(
            boost::asio::buffer(read_stage),
            boost::asio::use_awaitable);
        frame_assembler.AppendBytes(read_stage.data(), received);
    }
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_FRAME_RECEIVE_LOOP_H

#ifndef DAS_CORE_IPC_TEST_OUTBOUND_FRAME_PROBE_H
#define DAS_CORE_IPC_TEST_OUTBOUND_FRAME_PROBE_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/Utils/fmt.h>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>

// winsock 符号依赖 AnyTransport 的 boost 头先行引入，独立块防止排序前置
#include <das/Core/IPC/AfUnixAvailable.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
inline DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
#endif

/// 出站帧计数探针（Wave 0 观测 seam）：封装一对真实 pipe transport，
/// 对端交还给发送方（ConnectionManager::RegisterHostLocalTransport 或测试
/// 直接写入），本探针端逐帧循环读（每帧 32B header 自描述 body_size）——
/// pipe FIFO 无丢失，对端逐帧读出的帧数即发送方出站帧数的精确镜像。
/// 按 interface_id 计数并保留帧序记录（供 FIFO 断言），供 D-21 帧数基线 /
/// T10 / T7' 断言共用。夹具只依赖既有 transport 公开面，不新增生产代码
/// 观测点。
class OutboundFrameProbe
{
public:
    /// 一帧的完整记录（header + body，到达序）
    struct FrameRecord
    {
        DAS::Core::IPC::ValidatedIPCMessageHeader header;
        std::vector<uint8_t>                      body;
    };

    /// @param io_context transport 绑定的 io_context（独立使用传测试自建
    /// 的；挂在 IpcRunLoop 上传 run loop 的 GetIoContext()——排空时会按需
    /// run 该 io_context，顺带驱动 PostSend 的发送协程）
    /// @param io_context_externally_driven true 表示 io_context 由外部线程
    /// 持续驱动（如 StartRunLoop 后）——发送/排空只投递协程等结果，不再
    /// 自行 run/join（自行 join 会因外部 work guard 永不返回）
    explicit OutboundFrameProbe(
        boost::asio::io_context& io_context,
        bool                     io_context_externally_driven = false)
        : io_context_(io_context),
          io_context_externally_driven_(io_context_externally_driven)
    {
        // 连接对创建时 io_context 尚未被外部驱动（构造先于 StartRunLoop），
        // 自行 run 收敛安全
        CreateConnectedPair();
        // run() 返回后 io_context 处于 stopped 态；外部驱动模式下必须
        // restart——否则随后的 IpcRunLoop::Run() 里 run() 对 stopped
        // context 立即返回（io 永不执行，发送 future 永不就绪）。与
        // IpcRunLoopTest::CreateConnectedTransportPair 收尾的 restart 同理
        if (io_context_externally_driven_ && io_context_.stopped())
        {
            io_context_.restart();
        }
    }

    ~OutboundFrameProbe() { Shutdown(); }

    OutboundFrameProbe(const OutboundFrameProbe&) = delete;
    OutboundFrameProbe& operator=(const OutboundFrameProbe&) = delete;
    OutboundFrameProbe(OutboundFrameProbe&&) = delete;
    OutboundFrameProbe& operator=(OutboundFrameProbe&&) = delete;

    /// 连接对是否创建成功（失败时后续 API 不可用，测试应 ASSERT）
    [[nodiscard]]
    bool IsHealthy() const
    {
        return counterpart_side_transport_.has_value()
               && observer_side_transport_.has_value();
    }

    /// 取走对端 transport（一次性）：交给被观测的发送方（如
    /// ConnectionManager::RegisterHostLocalTransport）。从这一端发出的
    /// 全部帧都由本探针在观察端逐帧读出
    [[nodiscard]]
    std::optional<DAS::Core::IPC::AnyTransport> TakeCounterpartTransport()
    {
        if (!counterpart_side_transport_.has_value())
        {
            return std::nullopt;
        }
        return std::move(counterpart_side_transport_);
    }

    /// 从对端侧投递一帧（自证用：模拟发送方从对端出帧——探针随后在
    /// 观察端读回；对端被 TakeCounterpartTransport 取走后不可用）
    DasResult SendFrameFromCounterpart(
        const DAS::Core::IPC::ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&                      body)
    {
        if (!counterpart_side_transport_.has_value())
        {
            return DAS_E_IPC_NOT_INITIALIZED;
        }
        return SendVia(*counterpart_side_transport_, header, body);
    }

    /// 从探针观察端投递一帧（注入用：驱动对端（如 run loop 注册的接收
    /// 侧）的接收方向）
    DasResult SendFrameFromObserver(
        const DAS::Core::IPC::ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&                      body)
    {
        if (!observer_side_transport_.has_value())
        {
            return DAS_E_IPC_NOT_INITIALIZED;
        }
        return SendVia(*observer_side_transport_, header, body);
    }

    /// 排空接收窗口：窗口内持续逐帧读（deadline 保证协程必然收敛），
    /// 返回本窗口读到的帧数
    size_t DrainFor(std::chrono::milliseconds window)
    {
        return DrainUntil(SIZE_MAX, window);
    }

    /// 逐帧读直到凑满 expected_frame_count 帧或窗口耗尽（先到为准），
    /// 返回实际读到的帧数
    size_t DrainUntil(
        size_t                    expected_frame_count,
        std::chrono::milliseconds window)
    {
        if (!observer_side_transport_.has_value())
        {
            return 0;
        }
        auto drain_future = boost::asio::co_spawn(
            io_context_,
            DrainCoroutine(expected_frame_count, window),
            boost::asio::use_future);
        RunIoContextUntilSettled();
        return drain_future.get();
    }

    /// 清空观测记录（帧计数/帧序归零；管道字节不可回退，仅清观测面）
    void Reset()
    {
        frames_.clear();
        interface_id_order_.clear();
    }

    /// 累计读到的总帧数
    [[nodiscard]]
    size_t TotalFrameCount() const
    {
        return frames_.size();
    }

    /// 按 interface_id 的帧计数（如 130 = RemoteAddRef 独立帧数）
    [[nodiscard]]
    size_t CountByInterfaceId(uint32_t interface_id) const
    {
        return static_cast<size_t>(std::count_if(
            frames_.begin(),
            frames_.end(),
            [interface_id](const FrameRecord& frame)
            { return frame.header.GetInterfaceId() == interface_id; }));
    }

    /// 全部帧记录（到达序，供 FIFO / header 断言）
    [[nodiscard]]
    const std::vector<FrameRecord>& Frames() const
    {
        return frames_;
    }

    /// 帧序记录（interface_id 到达序，供 FIFO 断言）
    [[nodiscard]]
    const std::vector<uint32_t>& InterfaceIdOrder() const
    {
        return interface_id_order_;
    }

    /// 取指定 interface_id 的全部帧（dissect——如 130 号帧的 body/target
    /// 逐项检查）
    [[nodiscard]]
    std::vector<const FrameRecord*> FramesByInterfaceId(
        uint32_t interface_id) const
    {
        std::vector<const FrameRecord*> matched;
        for (const FrameRecord& frame : frames_)
        {
            if (frame.header.GetInterfaceId() == interface_id)
            {
                matched.push_back(&frame);
            }
        }
        return matched;
    }

    /// 关闭两端 transport（挂在 IpcRunLoop 上时必须在 run loop 析构前调
    /// 用，避免 transport 析构触碰已死的 io_context）
    void Shutdown()
    {
        if (counterpart_side_transport_.has_value())
        {
            counterpart_side_transport_->Cleanup();
            counterpart_side_transport_.reset();
        }
        if (observer_side_transport_.has_value())
        {
            observer_side_transport_->Cleanup();
            observer_side_transport_.reset();
        }
    }

private:
    /// 逐帧读协程：每轮一帧（32B header + 自描述 body_size），与剩余窗口
    /// 的 timer 竞速——deadline 到时由 timer 分支收敛，协程必然完成
    boost::asio::awaitable<size_t> DrainCoroutine(
        size_t                    expected_frame_count,
        std::chrono::milliseconds window)
    {
        using namespace boost::asio::experimental::awaitable_operators;

        size_t     received_count = 0;
        const auto deadline = std::chrono::steady_clock::now() + window;

        while (received_count < expected_frame_count)
        {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero())
            {
                break;
            }

            boost::asio::steady_timer deadline_timer(
                co_await boost::asio::this_coro::executor);
            deadline_timer.expires_after(remaining);

            auto receive_branch = [](DAS::Core::IPC::AnyTransport& transport)
                -> boost::asio::awaitable<
                    std::optional<DAS::Core::IPC::AsyncIpcMessage>>
            {
                auto received = co_await transport.ReceiveCoroutine();
                if (auto* error_code = std::get_if<DasResult>(&received))
                {
                    (void)error_code;
                    co_return std::nullopt;
                }
                co_return std::get<DAS::Core::IPC::AsyncIpcMessage>(
                    std::move(received));
            };
            auto deadline_branch = [&deadline_timer]()
                -> boost::asio::awaitable<
                    std::optional<DAS::Core::IPC::AsyncIpcMessage>>
            {
                co_await deadline_timer.async_wait(boost::asio::use_awaitable);
                co_return std::nullopt;
            };

            auto winner = co_await (
                receive_branch(*observer_side_transport_) || deadline_branch());
            if (winner.index() != 0)
            {
                // deadline 分支先收敛：窗口耗尽（或读分支出错返回 nullopt
                // 之外的竞态形态），停止排空
                break;
            }
            auto message = std::get<0>(std::move(winner));
            if (!message.has_value())
            {
                // 读分支以错误码收敛（对端关闭等）：停止排空
                break;
            }
            RecordFrame(std::move(*message));
            ++received_count;
        }
        co_return received_count;
    }

    void RecordFrame(DAS::Core::IPC::AsyncIpcMessage&& message)
    {
        frames_.push_back(
            FrameRecord{message.first, std::move(message.second)});
        interface_id_order_.push_back(message.first.GetInterfaceId());
    }

    /// 经指定端投递一帧（io_context 按需 run 驱动发送协程收敛）
    DasResult SendVia(
        DAS::Core::IPC::AnyTransport&                    transport,
        const DAS::Core::IPC::ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&                      body)
    {
        auto send_future = boost::asio::co_spawn(
            io_context_,
            transport.SendCoroutine(header, body.data(), body.size()),
            boost::asio::use_future);
        RunIoContextUntilSettled();
        return send_future.get();
    }

    /// 按需驱动 io_context 直到已投递的协程全部完成（同
    /// ProxyFactoryTest::ReceiveNextSentMessage 手法）；外部驱动模式下
    /// 跳过（io 由外部线程持续运行，此处只等待 future）——除非
    /// force_self_drive（连接对创建专用：外部驱动模式要求夹具构造先于
    /// StartRunLoop，构造时 io 尚无外部 runner/队列工作，自驱必然收敛）
    void RunIoContextUntilSettled(bool force_self_drive = false)
    {
        if (io_context_externally_driven_ && !force_self_drive)
        {
            return;
        }
        if (io_context_.stopped())
        {
            io_context_.restart();
        }
        std::thread io_thread([this]() { io_context_.run(); });
        io_thread.join();
    }

    /// 创建一对连通 transport（照 IpcAsyncTransportShmTest /
    /// IpcRunLoopTest::CreateConnectedTransportPair 的管道对创建手法）
    void CreateConnectedPair()
    {
        static std::atomic<uint32_t> next_endpoint_id{1};
        const auto                   base = DAS_FMT_NS::format(
            "ofp_{}_{}",
            GetCurrentProcessId() % 10000,
            next_endpoint_id.fetch_add(1));
        const auto read_endpoint = base + "_read";
        const auto write_endpoint = base + "_write";
        const bool use_single_endpoint = DAS::Core::IPC::AfUnixAvailable();
        const auto counterpart_read_endpoint =
            use_single_endpoint ? read_endpoint : write_endpoint;
        const auto counterpart_write_endpoint =
            use_single_endpoint ? write_endpoint : read_endpoint;

        auto counterpart_future = boost::asio::co_spawn(
            io_context_,
            DAS::Core::IPC::AnyTransport::CreateAsync(
                io_context_,
                read_endpoint,
                write_endpoint,
                true,
                65536),
            boost::asio::use_future);
        auto observer_future = boost::asio::co_spawn(
            io_context_,
            DAS::Core::IPC::AnyTransport::CreateAsync(
                io_context_,
                counterpart_read_endpoint,
                counterpart_write_endpoint,
                false,
                65536),
            boost::asio::use_future);

        RunIoContextUntilSettled(true);

        auto [counterpart_result, counterpart_transport] =
            counterpart_future.get();
        auto [observer_result, observer_transport] = observer_future.get();

        if (counterpart_result != DAS_S_OK || !counterpart_transport)
        {
            return;
        }
        if (observer_result != DAS_S_OK || !observer_transport)
        {
            return;
        }

        counterpart_side_transport_ = std::move(*counterpart_transport);
        observer_side_transport_ = std::move(*observer_transport);
    }

    boost::asio::io_context& io_context_;

    /// io_context 是否由外部线程持续驱动（外部驱动模式下不自行 run/join）
    bool io_context_externally_driven_ = false;

    /// 对端（被观测的发送方一端；等待被 TakeCounterpartTransport 取走，
    /// 取走前可用 SendFrameFromCounterpart 自证）
    std::optional<DAS::Core::IPC::AnyTransport> counterpart_side_transport_;

    /// 探针观察端（逐帧读出站镜像 + 可向对端注入入站帧）
    std::optional<DAS::Core::IPC::AnyTransport> observer_side_transport_;

    /// 帧记录（到达序）
    std::vector<FrameRecord> frames_;

    /// 帧序记录（interface_id 到达序）
    std::vector<uint32_t> interface_id_order_;
};

#endif // DAS_CORE_IPC_TEST_OUTBOUND_FRAME_PROBE_H

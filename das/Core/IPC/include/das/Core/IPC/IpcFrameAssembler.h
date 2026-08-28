#ifndef DAS_CORE_IPC_FRAME_ASSEMBLER_H
#define DAS_CORE_IPC_FRAME_ASSEMBLER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/DasConfig.h>
#include <das/Utils/fmt.h>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_SML_WARNING
#include <boost/sml.hpp>
DAS_DISABLE_WARNING_END

DAS_CORE_IPC_NS_BEGIN

namespace detail
{
    /// @brief 帧重组数据面：缓冲、预检缓存、出帧队列与失败态
    ///
    /// 独立成结构的理由：迁移表 lambda 以本类型为依赖，sm 按值内嵌
    /// 在宿主类成员声明点实例化时 lambda 体就要可编译——依赖类型在
    /// 该点必须完整（宿主类自身做不到，它此刻不完整）。framing 原语
    /// 全部落在 Context 上，guard/action 与宿主类共用一份逻辑
    struct FrameAssemblerContext
    {
        size_t max_body_size = 65536;
        bool   failed = false;

        std::string failure_description;

        std::vector<uint8_t>        pending_bytes;
        std::deque<AsyncIpcMessage> complete_frames;

        ValidatedIPCMessageHeader peeked_header{};
        bool                      has_peeked_header = false;
        ValidatedIPCMessageHeader current_header{};

        /// 预检并缓存 header（guard 共用；结果供 ParseHeader 消费）
        void PeekHeader()
        {
            has_peeked_header = false;

            if (pending_bytes.size() < sizeof(IPCMessageHeader))
            {
                return;
            }

            HeaderValidationResult validation_error;
            auto peeked = ValidatedIPCMessageHeader::Deserialize(
                pending_bytes.data(),
                sizeof(IPCMessageHeader),
                &validation_error);

            if (peeked.has_value())
            {
                peeked_header = *peeked;
                has_peeked_header = true;
                failure_description.clear();
            }
            else
            {
                failure_description.assign(validation_error.message);
            }
        }

        /// guard：字节不足恒 false；齐则校验，非法或超限 true
        bool HeaderFailsValidation()
        {
            if (pending_bytes.size() < sizeof(IPCMessageHeader))
            {
                return false;
            }

            PeekHeader();
            if (!has_peeked_header)
            {
                return true;
            }

            if (peeked_header.Raw().body_size > max_body_size)
            {
                failure_description = DAS_FMT_NS::format(
                    "Message body too large: body_size = {}, max = {}",
                    peeked_header.Raw().body_size,
                    max_body_size);
                return true;
            }
            return false;
        }

        /// guard：缓冲字节是否覆盖一个合法 header
        bool HeaderComplete()
        {
            if (!has_peeked_header)
            {
                PeekHeader();
            }
            return has_peeked_header;
        }

        /// guard：缓冲字节是否覆盖当前帧 body
        bool BodyComplete()
        {
            return pending_bytes.size()
                   >= sizeof(IPCMessageHeader)
                          + current_header.Raw().body_size;
        }

        /// action：把缓存 header 固化为当前帧 header
        void ParseHeader()
        {
            if (!has_peeked_header)
            {
                PeekHeader();
            }
            current_header = peeked_header;
            has_peeked_header = false;
        }

        /// action：拷出 header+body 成帧入队，消费缓冲前缀
        void EmitFrame()
        {
            const auto body_size = current_header.Raw().body_size;

            std::vector<uint8_t> body(body_size);
            if (body_size > 0)
            {
                std::memcpy(
                    body.data(),
                    pending_bytes.data() + sizeof(IPCMessageHeader),
                    body_size);
            }

            complete_frames.emplace_back(current_header, std::move(body));
            pending_bytes.erase(
                pending_bytes.begin(),
                pending_bytes.begin()
                    + static_cast<std::ptrdiff_t>(
                        sizeof(IPCMessageHeader) + body_size));
        }

        /// action：记录失败原因（含原始字节 hex，与旧读路径日志同构）
        void RecordHeaderFailure()
        {
            failed = true;

            std::string       hex_bytes;
            const std::size_t dump_size =
                std::min(pending_bytes.size(), sizeof(IPCMessageHeader));
            for (std::size_t index = 0; index < dump_size; ++index)
            {
                hex_bytes +=
                    DAS_FMT_NS::format("{:02X} ", pending_bytes[index]);
            }
            failure_description = DAS_FMT_NS::format(
                "Header validation failed: {}, raw bytes: [{}]",
                failure_description,
                hex_bytes);
        }
    };
} // namespace detail

/// 追加一批 read_some 字节（事件：驱动状态机迁移；命名空间作用域——
/// 迁移表在宿主类前向声明期就要引用它）
struct FrameBytesAppended
{
};

/// @brief 帧重组状态机迁移表（依赖 detail::FrameAssemblerContext——
/// 完整类型，lambda 体在 sm 实例化点即可编译）
///
/// 事件行 + 匿名行。匿名行在事件迁移落位后立即评估，构成缓冲内连续
/// 完整帧的排空链——一次 AppendBytes 吃光整批合写帧，无需重复投递
/// 事件。guard 预检缓存的 header 由同轮 action 消费，避免重复反序列化
struct IpcFrameAssemblerMachine
{
    auto operator()() const noexcept
    {
        using namespace boost::sml;
        return make_transition_table(
            // header 字节齐且校验失败/超限 → 失败终态
            *state<class AwaitingHeaderState>
                + event<FrameBytesAppended>[(
                      [](detail::FrameAssemblerContext& context) -> bool
                      { return context.HeaderFailsValidation(); })]
                      / [](detail::FrameAssemblerContext& context)
                      { context.RecordHeaderFailure(); } =
                state<class FailedState>,
            // header 字节齐且合法 → 缓存 header，等 body
            state<class AwaitingHeaderState>
                + event<FrameBytesAppended>[(
                      [](detail::FrameAssemblerContext& context) -> bool
                      { return context.HeaderComplete(); })]
                      / [](detail::FrameAssemblerContext& context)
                      { context.ParseHeader(); } =
                state<class AwaitingBodyState>,
            // body 字节齐 → 出帧，回 AwaitingHeader
            state<class AwaitingBodyState>
                + event<FrameBytesAppended>[(
                      [](detail::FrameAssemblerContext& context) -> bool
                      { return context.BodyComplete(); })]
                      / [](detail::FrameAssemblerContext& context)
                      { context.EmitFrame(); } =
                state<class AwaitingHeaderState>,
            // 匿名排空链（无事件行，落位后立即评估）：继续吃缓冲里的
            // 连续完整帧
            state<class AwaitingHeaderState>[(
                [](detail::FrameAssemblerContext& context) -> bool
                { return context.HeaderFailsValidation(); })]
                / [](detail::FrameAssemblerContext& context)
            { context.RecordHeaderFailure(); } = state<class FailedState>,
            state<class AwaitingHeaderState>[(
                [](detail::FrameAssemblerContext& context) -> bool
                { return context.HeaderComplete(); })]
                / [](detail::FrameAssemblerContext& context)
            { context.ParseHeader(); } = state<class AwaitingBodyState>,
            state<class AwaitingBodyState>[(
                [](detail::FrameAssemblerContext& context) -> bool
                { return context.BodyComplete(); })]
                / [](detail::FrameAssemblerContext& context)
            { context.EmitFrame(); } = state<class AwaitingHeaderState>);
    }
};

/// @brief 单次读帧重组器（boost::sml 状态机，按值内嵌无堆间接）
///
/// 接收侧把「恰好读 header 字节 + 恰好读 body 字节」的两次 overlapped
/// 读（各付一次 IOCP 完成分发）换成 read_some 收多少吃多少：字节追加
/// 进缓冲驱动状态机，完整帧入队，撕裂部分留存等下一批。合写批次
/// （一次 WriteFile 多帧）经一次 read_some 全量落入后，由匿名迁移链
/// 逐帧排空——后续帧零系统调用。
///
/// 状态机只管帧边界（framing）：header 校验、body_size 上限裁决与
/// 失败终态；SHM 大帧重组、业务路由由调用方在取出的帧上处理。
///
/// 生命周期：单连接串行使用（与既有接收协程的串行约束一致）。机器
/// 持有 context 引用，拷贝删除；移动经「context 迁移 + 机器重绑定」
/// 实现，被移出方弃用。
class IpcFrameAssembler
{
public:
    /// @param max_body_size body_size 上限（对齐连接的 max_message_size，
    /// 超限按非法帧终态处理——与旧读路径的拒绝语义一致）
    explicit IpcFrameAssembler(size_t max_body_size) : machine_{context_}
    {
        context_.max_body_size = max_body_size;
    }

    IpcFrameAssembler(const IpcFrameAssembler&) = delete;
    IpcFrameAssembler& operator=(const IpcFrameAssembler&) = delete;

    IpcFrameAssembler(IpcFrameAssembler&& other) noexcept
        : context_(std::move(other.context_)), machine_{context_}
    {
    }

    IpcFrameAssembler& operator=(IpcFrameAssembler&& other) noexcept
    {
        if (this != &other)
        {
            context_ = std::move(other.context_);
            // 机器持有 context 引用且不可赋值：析构后原地重建绑定到本
            // 对象（被移出方弃用）。机器状态位随重建回到初始态——本
            // 状态机的全部状态可由 pending_bytes 重新推导（header 到
            // EmitFrame 才消费，ParseHeader 幂等；Failed 态由
            // context.failed 旗标独立承载），下一批字节即自愈，
            // 不构成语义丢失。给机器新增不可推导的状态前须重审此处
            std::destroy_at(&machine_);
            std::construct_at(&machine_, context_);
        }
        return *this;
    }

    /// @brief 追加一批字节并驱动状态机；完整帧随后可经 TryTakeFrame
    /// 逐个取出（FIFO，含此前批次残留）
    void AppendBytes(const uint8_t* data, size_t size)
    {
        context_.pending_bytes.insert(
            context_.pending_bytes.end(),
            data,
            data + size);
        machine_.process_event(FrameBytesAppended{});
    }

    /// @brief 取出一帧；无完整帧时返回 false
    bool TryTakeFrame(AsyncIpcMessage& out_frame)
    {
        if (context_.complete_frames.empty())
        {
            return false;
        }
        out_frame = std::move(context_.complete_frames.front());
        context_.complete_frames.pop_front();
        return true;
    }

    /// @brief 状态机是否进入失败终态（非法 header / 超限 body）
    /// 调用方应按 INVALID_MESSAGE 处理并断开连接
    [[nodiscard]]
    bool HasFailed() const noexcept
    {
        return context_.failed;
    }

    /// @brief 失败原因（含原始 header 字节 hex，供日志对照；成功路径为空）
    [[nodiscard]]
    const std::string& FailureDescription() const noexcept
    {
        return context_.failure_description;
    }

    /// @brief body_size 上限（staging 缓冲尺寸对齐用）
    [[nodiscard]]
    size_t GetMaxBodySize() const noexcept
    {
        return context_.max_body_size;
    }

    /// @brief 调整 body_size 上限；仅在首个字节追加前调用（连接建立
    /// 期配置，运行中改限会撕裂已缓存的帧边界判定）
    void SetMaxBodySize(size_t max_body_size)
    {
        context_.max_body_size = max_body_size;
    }

private:
    // 声明顺序即绑定顺序：context 先在，机器引用它
    detail::FrameAssemblerContext            context_;
    boost::sml::sm<IpcFrameAssemblerMachine> machine_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_FRAME_ASSEMBLER_H

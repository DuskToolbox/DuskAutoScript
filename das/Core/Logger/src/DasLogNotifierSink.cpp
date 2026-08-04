// DasLogNotifierSink: 多继承 spdlog base_sink<null_mutex> + IDasLogNotifier
// 的进程级固定 sink。sink_it_ 在每条 DAS_LOG 触发时置 Spdlog dirty；recorder
// 写完 user.jsonl 后调 Signal(UserJsonl) 置 UserJsonl dirty。Http 16ms 定时器
// 轮询 Consume(channel) 拿到 true 时 hub->Broadcast(tick)。
//
// 借用引用语义（借用引用语义）：g_logger 的 shared_ptr 拥有此 sink
// 整个进程生命周期； AddRef/Release 空操作，DasPtr<IDasLogNotifier> 持有期间
// sink 永远有效。
#include <das/Core/Logger/DasLogNotifierSink.h>
#include <das/DasTypes.hpp>

namespace Das::Core
{
    thread_local int DasLogNotifierSink::reentry_depth_ = 0;

    void DasLogNotifierSink::sink_it_(const spdlog::details::log_msg& msg)
    {
        // 防 DAS_LOG 在 sink_it_ body 内递归（如 sink_it_ 内调
        // DAS_CORE_LOG_ERROR 会再次进 sink_it_ 进 same sink）。reentry_depth_
        // 进入 +1 / 退出 -1，>0 时早退。 thread_local：同线程独立计数，跨线程的
        // DAS_LOG 互不影响（各自正常置 dirty）。
        if (reentry_depth_ > 0)
        {
            return;
        }
        ++reentry_depth_;
        dirty_spdlog_.store(true, std::memory_order_release);
        --reentry_depth_;
        // msg 引用未使用——broker 不写盘，只置 dirty flag。
        (void)msg;
    }

    void DasLogNotifierSink::flush_()
    {
        // broker 不缓冲任何数据，flush 是 no-op（spdlog rotating_file_sink
        // 自己负责落盘）。
    }

    uint32_t DasLogNotifierSink::AddRef()
    {
        // 借用引用：g_logger 的 shared_ptr 拥有此 sink 生命周期，外部 DasPtr
        // 持有是借用。 返回 1（非零）以与 DasPtr 期望（非零代表 still
        // alive）兼容；永不析构此对象。
        return 1;
    }

    uint32_t DasLogNotifierSink::Release()
    {
        // 同 AddRef：空操作，永不析构（g_logger shared_ptr 拥有）。
        return 1;
    }

    DasResult DasLogNotifierSink::QueryInterface(
        const DasGuid& iid,
        void**         pp_out_object)
    {
        if (pp_out_object == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (iid == DasIidOf<IDasBase>())
        {
            *pp_out_object = static_cast<IDasBase*>(
                static_cast<Das::ExportInterface::IDasLogNotifier*>(this));
            return DAS_S_OK;
        }
        if (iid == DasIidOf<Das::ExportInterface::IDasLogNotifier>())
        {
            *pp_out_object =
                static_cast<Das::ExportInterface::IDasLogNotifier*>(this);
            return DAS_S_OK;
        }
        *pp_out_object = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    DasResult DasLogNotifierSink::Signal(
        Das::ExportInterface::DasLogChannel channel)
    {
        switch (channel)
        {
        case Das::ExportInterface::DAS_LOG_CHANNEL_SPDLOG:
            dirty_spdlog_.store(true, std::memory_order_release);
            return DAS_S_OK;
        case Das::ExportInterface::DAS_LOG_CHANNEL_USER_JSONL:
            dirty_user_jsonl_.store(true, std::memory_order_release);
            return DAS_S_OK;
        default:
            return DAS_E_INVALID_ARGUMENT;
        }
    }

    DasResult DasLogNotifierSink::Consume(
        Das::ExportInterface::DasLogChannel channel,
        bool*                               p_out_was_dirty)
    {
        if (p_out_was_dirty == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        switch (channel)
        {
        case Das::ExportInterface::DAS_LOG_CHANNEL_SPDLOG:
            *p_out_was_dirty =
                dirty_spdlog_.exchange(false, std::memory_order_acq_rel);
            return DAS_S_OK;
        case Das::ExportInterface::DAS_LOG_CHANNEL_USER_JSONL:
            *p_out_was_dirty =
                dirty_user_jsonl_.exchange(false, std::memory_order_acq_rel);
            return DAS_S_OK;
        default:
            return DAS_E_INVALID_ARGUMENT;
        }
    }
} // namespace Das::Core

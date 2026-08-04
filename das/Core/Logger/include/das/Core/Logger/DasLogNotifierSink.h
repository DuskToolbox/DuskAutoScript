#ifndef DAS_CORE_LOGGER_DASLOGNOTIFIERSINK_H
#define DAS_CORE_LOGGER_DASLOGNOTIFIERSINK_H

#include <das/_autogen/idl/abi/IDasLogRecorder.h>

#include <atomic>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/sink.h>

namespace Das::Core
{
    // 统一日志通知 sink：多继承 spdlog base_sink<null_mutex>（线程安全 sink_it_
    // 串行化）
    // + IDasLogNotifier（per-channel atomic dirty flag）。进程级固定挂载在
    // g_logger 的 sinks vector 中（与 stdout / rotating_file
    // 并列，构造期塞入，运行时永不增删—— vector 固定使 sink_it_
    // 的无锁迭代安全，vector 固定使无锁迭代安全）。。
    //
    // AddRef/Release 是借用引用的空操作：sink 的生命周期由 g_logger 的
    // shared_ptr 拥有， 进程级永不死；Http 持 DasPtr<IDasLogNotifier> 期间 sink
    // 永远有效（借用引用语义）。
    //
    // 线程模型：base_sink<null_mutex> 的 sink_it_ / flush_ 自带互斥（null_mutex
    // 用于 单线程 logger；g_logger 是 shared_ptr 跨线程，但 spdlog
    // logger::sink_it_ 已经 在调用 base_sink 的 lock_ 后才进 sink_it_，所以
    // sink_it_ body 内修改 atomic 是安全的——atomic 本身也独立线程安全）。
    class DasLogNotifierSink final
        : public spdlog::sinks::base_sink<spdlog::details::null_mutex>,
          public Das::ExportInterface::IDasLogNotifier
    {
        // 防 DAS_LOG 在 sink_it_ 内部递归（递归会再次进 sink_it_ 进 same
        // sink）。 thread_local：每个线程独立计数器，进入时 +1，退出时 -1；>0
        // 时 sink_it_ 早退。 不用 std::atomic<bool> 是因为同线程的 sink_it_
        // 嵌套调用要计数到 0 才放行， 单 bool 无法区分「已在
        // sink_it_」与「刚退出」。
        thread_local static int reentry_depth_;

        std::atomic<bool> dirty_spdlog_{false};
        std::atomic<bool> dirty_user_jsonl_{false};

    protected:
        // sink_it_ 由 spdlog logger 在 lock_ 持有下调用（base_sink 串行化）。
        // 置 Spdlog dirty flag，让 Http 16ms 定时器的 Consume(Spdlog) 拿到
        // true。 thread_local reentry guard 防递归：sink_it_ 内若 DAS_LOG
        // 会再进 sink_it_， 此时 reentry_depth_ > 0，早退不再置
        // dirty（避免无限递归）。
        void sink_it_(const spdlog::details::log_msg& msg) override;
        void flush_() override;

    public:
        DasLogNotifierSink() = default;
        ~DasLogNotifierSink() override = default;

        // ===== IDasBase 借用引用语义（借用引用语义）=====
        // g_logger 的 shared_ptr 拥有此 sink 整个进程生命周期；任何 DasPtr
        // 持有是借用。 AddRef/Release 空操作返回
        // 1（既不离群析构也不泄漏）；QueryInterface 仅支持 IDasLogNotifier /
        // IDasBase 两个 iid。
        uint32_t  AddRef() override;
        uint32_t  Release() override;
        DasResult QueryInterface(const DasGuid& iid, void** pp_out_object)
            override;

        // ===== IDasLogNotifier =====
        DasResult Signal(Das::ExportInterface::DasLogChannel channel) override;
        DasResult Consume(
            Das::ExportInterface::DasLogChannel channel,
            bool*                               p_out_was_dirty) override;
    };

} // namespace Das::Core

#endif // DAS_CORE_LOGGER_DASLOGNOTIFIERSINK_H

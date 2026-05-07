#ifndef DAS_CORE_DEBUG_DEBUGWRITERIMPL_H
#define DAS_CORE_DEBUG_DEBUGWRITERIMPL_H

#include <das/Core/Debug/DebugEvent.h>
#include <das/Core/Debug/DebugSink.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasDebugWriter.Implements.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

DAS_CORE_DEBUG_NS_BEGIN

class DebugWriterImpl final
    : public Das::ExportInterface::DasDebugWriterImplBase<DebugWriterImpl>,
      public IDebugSink,
      public IDebugDrain
{
public:
    explicit DebugWriterImpl(std::filesystem::path debug_dir);
    ~DebugWriterImpl() override;

    DasResult DAS_STD_CALL LogEntry(IDasReadOnlyString* p_event_json) override;
    DasResult DAS_STD_CALL Flush() override;

    DasResult Submit(const DebugEvent& event) override;
    void Shutdown() override;

private:
    struct QueueItem
    {
        DebugEvent event;
        std::string raw_json;
        bool        has_raw_json{false};
    };

    DasResult Enqueue(QueueItem item);
    void WorkerLoop();
    DasResult WriteOne(const QueueItem& item, uint64_t step);

    std::filesystem::path debug_dir_;
    std::filesystem::path jsonl_path_;
    std::mutex            mutex_;
    std::condition_variable cv_;
    std::deque<QueueItem> queue_;
    bool                  stopping_{false};
    bool                  worker_started_{false};
    bool                  writer_active_{false};
    uint64_t              next_step_{1};
    std::thread           worker_;
};

DasResult RegisterDebugWriterService(
    Das::Core::IPC::MainProcess::IIpcContext& ipc_context);

DAS_CORE_DEBUG_NS_END

#endif // DAS_CORE_DEBUG_DEBUGWRITERIMPL_H

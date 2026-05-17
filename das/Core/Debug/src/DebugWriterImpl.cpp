#define _CRT_SECURE_NO_WARNINGS

#include "DebugWriterImpl.h"

#include <das/Core/Debug/DebugRuntime.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>

#include <chrono>
#include <exception>
#include <fstream>
#include <iomanip>
#include <new>
#include <optional>
#include <sstream>
#include <utility>

#ifdef DAS_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#endif

DAS_CORE_DEBUG_NS_BEGIN
namespace
{
    constexpr auto kDebugWriterServiceName = "debug.writer";
    constexpr auto kMaxQueuedEvents = 4096U;

    auto NowIsoString() -> std::string
    {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm    utc{};
#ifdef DAS_WINDOWS
        gmtime_s(&utc, &time);
#else
        gmtime_r(&time, &utc);
#endif
        std::ostringstream stream;
        stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return stream.str();
    }

    auto CurrentThreadIdString() -> std::string
    {
        std::ostringstream stream;
        stream << std::this_thread::get_id();
        return stream.str();
    }

    uint64_t CurrentProcessId()
    {
#ifdef DAS_WINDOWS
        return static_cast<uint64_t>(::GetCurrentProcessId());
#else
        return static_cast<uint64_t>(::getpid());
#endif
    }

    auto JsonOrEmptyObject(const std::string& json) -> yyjson::value
    {
        auto parsed = DAS::Utils::ParseYyjsonFromString(json);
        return parsed ? std::move(*parsed) : DAS::Utils::MakeYyjsonObject();
    }

    auto RawParamsJson(const std::string& raw_json) -> std::string
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("raw_event_json")] = raw_json;
        auto serialized = DAS::Utils::SerializeYyjsonValue(obj);
        return serialized.value_or("{}");
    }

    auto StringField(
        yyjson::value&     value,
        std::string_view   key,
        const std::string& fallback) -> std::string
    {
        auto object = value.as_object();
        if (!object || !object->contains(key))
        {
            return fallback;
        }
        auto field = (*object)[key].as_string();
        return field ? std::string{*field} : fallback;
    }

    auto JsonField(
        yyjson::value&     value,
        std::string_view   key,
        const std::string& fallback) -> std::string
    {
        auto object = value.as_object();
        if (!object || !object->contains(key))
        {
            return fallback;
        }

        yyjson::value field_copy;
        field_copy = (*object)[key];
        auto serialized = DAS::Utils::SerializeYyjsonValue(field_copy);
        return serialized.value_or(fallback);
    }

    auto DoubleField(
        yyjson::value&   value,
        std::string_view key,
        double           fallback) -> double
    {
        auto object = value.as_object();
        if (!object || !object->contains(key))
        {
            return fallback;
        }

        auto field = (*object)[key];
        if (auto real = field.as_real())
        {
            return *real;
        }
        if (auto sint = field.as_sint())
        {
            return static_cast<double>(*sint);
        }
        if (auto uint = field.as_uint())
        {
            return static_cast<double>(*uint);
        }
        return fallback;
    }

    auto EventFromRawJson(const std::string& raw_json) -> DebugEvent
    {
        auto event = MakeDebugEvent("raw", RawParamsJson(raw_json), "{}");
        auto parsed = DAS::Utils::ParseYyjsonFromString(raw_json);
        if (!parsed || !parsed->is_object())
        {
            return event;
        }

        event.type = StringField(*parsed, "type", event.type);
        event.timestamp = StringField(*parsed, "timestamp", event.timestamp);
        event.params_json = JsonField(*parsed, "params", event.params_json);
        event.result_json = JsonField(*parsed, "result", event.result_json);
        event.elapsed_ms = DoubleField(*parsed, "elapsed_ms", event.elapsed_ms);
        event.image_filename =
            StringField(*parsed, "image_filename", event.image_filename);
        return event;
    }

    auto SerializeForJsonl(const DebugEvent& event, uint64_t step)
        -> std::optional<std::string>
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("step")] =
            static_cast<uint64_t>(step);
        (*obj.as_object())[std::string_view("type")] = event.type;
        (*obj.as_object())[std::string_view("timestamp")] =
            event.timestamp.empty() ? NowIsoString() : event.timestamp;
        (*obj.as_object())[std::string_view("params")] =
            JsonOrEmptyObject(event.params_json);
        (*obj.as_object())[std::string_view("result")] =
            JsonOrEmptyObject(event.result_json);
        (*obj.as_object())[std::string_view("elapsed_ms")] = event.elapsed_ms;
        (*obj.as_object())[std::string_view("thread_id")] =
            CurrentThreadIdString();
        (*obj.as_object())[std::string_view("process_pid")] =
            CurrentProcessId();
        (*obj.as_object())[std::string_view("image_filename")] =
            event.image_filename;

        return DAS::Utils::SerializeYyjsonValue(obj);
    }

    std::shared_ptr<IDebugSink> MakeSinkRef(DebugWriterImpl* writer)
    {
        writer->AddRef();
        return std::shared_ptr<IDebugSink>(
            static_cast<IDebugSink*>(writer),
            [writer](IDebugSink*) { writer->Release(); });
    }

    std::shared_ptr<IDebugDrain> MakeDrainRef(DebugWriterImpl* writer)
    {
        writer->AddRef();
        return std::shared_ptr<IDebugDrain>(
            static_cast<IDebugDrain*>(writer),
            [writer](IDebugDrain*) { writer->Release(); });
    }

} // namespace

DebugWriterImpl::DebugWriterImpl(std::filesystem::path debug_dir)
    : debug_dir_(std::move(debug_dir)), jsonl_path_(debug_dir_ / "debug.jsonl")
{
    std::filesystem::create_directories(debug_dir_);
    worker_ = std::thread([this]() { WorkerLoop(); });
    worker_started_ = true;
}

DebugWriterImpl::~DebugWriterImpl() { Shutdown(); }

DasResult DebugWriterImpl::LogEntry(IDasReadOnlyString* p_event_json)
{
    if (!p_event_json)
    {
        return DAS_E_INVALID_POINTER;
    }

    const char* p_utf8 = nullptr;
    const auto  result = p_event_json->GetUtf8(&p_utf8);
    if (result < 0)
    {
        return result;
    }
    if (!p_utf8)
    {
        return DAS_E_INVALID_POINTER;
    }

    QueueItem item{};
    item.raw_json = p_utf8;
    item.has_raw_json = true;
    return Enqueue(std::move(item));
}

DasResult DebugWriterImpl::Submit(const DebugEvent& event)
{
    QueueItem item{};
    item.event = event;
    return Enqueue(std::move(item));
}

DasResult DebugWriterImpl::Enqueue(QueueItem item)
{
    std::unique_lock lock{mutex_};
    cv_.wait(
        lock,
        [this]() { return stopping_ || queue_.size() < kMaxQueuedEvents; });
    if (stopping_)
    {
        return DAS_E_OBJECT_NOT_INIT;
    }

    queue_.emplace_back(std::move(item));
    cv_.notify_all();
    return DAS_S_OK;
}

void DebugWriterImpl::WorkerLoop()
{
    for (;;)
    {
        QueueItem item{};
        uint64_t  step = 0;
        {
            std::unique_lock lock{mutex_};
            cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty())
            {
                return;
            }

            item = std::move(queue_.front());
            queue_.pop_front();
            writer_active_ = true;
            step = next_step_++;
        }

        DasResult result = DAS_S_OK;
        try
        {
            result = WriteOne(item, step);
        }
        catch (const std::bad_alloc& ex)
        {
            DAS_CORE_LOG_ERROR("Debug writer worker exception: {}", ex.what());
            result = DAS_E_OUT_OF_MEMORY;
        }
        catch (const std::exception& ex)
        {
            DAS_CORE_LOG_ERROR("Debug writer worker exception: {}", ex.what());
            result = DAS_E_FAIL;
        }
        catch (...)
        {
            DAS_CORE_LOG_ERROR("Debug writer worker unknown exception");
            result = DAS_E_FAIL;
        }

        {
            std::lock_guard lock{mutex_};
            if (result < 0 && worker_error_ >= 0)
            {
                worker_error_ = result;
            }
            writer_active_ = false;
        }
        cv_.notify_all();
    }
}

DasResult DebugWriterImpl::WriteOne(const QueueItem& item, uint64_t step)
{
    const auto event =
        item.has_raw_json ? EventFromRawJson(item.raw_json) : item.event;
    auto line = SerializeForJsonl(event, step);
    if (!line)
    {
        return DAS_E_INVALID_JSON;
    }

    std::ofstream output{jsonl_path_, std::ios::app | std::ios::binary};
    if (!output)
    {
        return DAS_E_INVALID_FILE;
    }
    output << *line << '\n';
    output.flush();
    return output ? DAS_S_OK : DAS_E_INVALID_FILE;
}

DasResult DebugWriterImpl::Flush()
{
    std::unique_lock lock{mutex_};
    cv_.wait(lock, [this]() { return queue_.empty() && !writer_active_; });
    const auto result = worker_error_;
    worker_error_ = DAS_S_OK;
    return result;
}

void DebugWriterImpl::Shutdown()
{
    {
        std::lock_guard lock{mutex_};
        if (stopping_)
        {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_started_ && worker_.joinable())
    {
        worker_.join();
    }
    worker_started_ = false;
}

DasResult RegisterDebugWriterService(
    Das::Core::IPC::MainProcess::IIpcContext& ipc_context)
{
    if (!DebugRuntime::IsEnabled())
    {
        return DAS_S_OK;
    }

    auto writer = Das::DasPtr<DebugWriterImpl>::Attach(
        DebugWriterImpl::MakeRaw(DebugRuntime::DebugDir()));
    auto result = ipc_context.RegisterServiceByName(
        writer.Get(),
        DasIidOf<Das::ExportInterface::IDasDebugWriter>(),
        kDebugWriterServiceName);
    if (result < 0)
    {
        return result;
    }

    DebugRuntime::RegisterSink(MakeSinkRef(writer.Get()));
    DebugRuntime::RegisterDrain(MakeDrainRef(writer.Get()));
    return DAS_S_OK;
}

DAS_CORE_DEBUG_NS_END

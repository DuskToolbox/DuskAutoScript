#include <das/Core/Debug/DebugImageAnnotator.h>

#include <das/Core/Debug/DebugRuntime.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/OcvWrapper/IImageBackend.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>
#include <das/_autogen/idl/abi/IDasBinaryBuffer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_OPENCV_WARNING
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
DAS_DISABLE_WARNING_END

DAS_CORE_DEBUG_NS_BEGIN
namespace
{
    struct ImageJob
    {
        cv::Mat               original;
        cv::Mat               annotated;
        std::filesystem::path original_path;
        std::filesystem::path annotated_path;
    };

    struct ImageWorkerState
    {
        std::mutex              mutex;
        std::condition_variable cv;
        std::deque<ImageJob>    queue;
        bool                    stopping{false};
        bool                    active{false};
        bool                    running{false};
        DasResult               first_error{DAS_S_OK};
        std::thread             worker;
    };

    ImageWorkerState& WorkerState()
    {
        static ImageWorkerState state;
        return state;
    }

    auto TimestampForFilename() -> std::string
    {
        static std::atomic<uint64_t> counter{0};
        const auto                   now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        const auto millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count()
            % 1000;
        std::tm utc{};
#ifdef DAS_WINDOWS
        gmtime_s(&utc, &time);
#else
        gmtime_r(&time, &utc);
#endif

        std::ostringstream stream;
        stream << std::put_time(&utc, "%Y%m%d_%H%M%S") << '_' << std::setw(3)
               << std::setfill('0') << millis << '_' << counter.fetch_add(1);
        return stream.str();
    }

    auto SanitizeStepName(const std::string& step_name) -> std::string
    {
        std::string result;
        result.reserve(step_name.size());
        for (const unsigned char ch : step_name)
        {
            if (std::isalnum(ch) || ch == '-' || ch == '_')
            {
                result.push_back(static_cast<char>(ch));
            }
            else
            {
                result.push_back('_');
            }
        }
        if (result.empty())
        {
            return "debug_step";
        }
        return result;
    }

    auto ExpectedChannelCount(Das::ExportInterface::DasImagePixelFormat format)
        -> int32_t
    {
        switch (format)
        {
        case Das::ExportInterface::DAS_PIXEL_FORMAT_BGR:
        case Das::ExportInterface::DAS_PIXEL_FORMAT_RGB:
        case Das::ExportInterface::DAS_PIXEL_FORMAT_HSV:
            return 3;
        case Das::ExportInterface::DAS_PIXEL_FORMAT_RGBA:
            return 4;
        case Das::ExportInterface::DAS_PIXEL_FORMAT_GRAY:
            return 1;
        default:
            return 0;
        }
    }

    auto ColorOf(DebugAnnotationColor color) -> cv::Scalar
    {
        switch (color)
        {
        case DebugAnnotationColor::Green:
            return {0, 255, 0};
        case DebugAnnotationColor::Yellow:
            return {0, 255, 255};
        case DebugAnnotationColor::Red:
            return {0, 0, 255};
        case DebugAnnotationColor::Blue:
            return {255, 0, 0};
        }
        return {0, 255, 0};
    }

    auto ConvertToBgr(
        const cv::Mat&                            src,
        Das::ExportInterface::DasImagePixelFormat format) -> cv::Mat
    {
        if (src.empty())
        {
            return {};
        }

        cv::Mat bgr;
        switch (format)
        {
        case Das::ExportInterface::DAS_PIXEL_FORMAT_BGR:
            bgr = src.clone();
            break;
        case Das::ExportInterface::DAS_PIXEL_FORMAT_RGB:
            cv::cvtColor(src, bgr, cv::COLOR_RGB2BGR);
            break;
        case Das::ExportInterface::DAS_PIXEL_FORMAT_RGBA:
            cv::cvtColor(src, bgr, cv::COLOR_RGBA2BGR);
            break;
        case Das::ExportInterface::DAS_PIXEL_FORMAT_GRAY:
            cv::cvtColor(src, bgr, cv::COLOR_GRAY2BGR);
            break;
        case Das::ExportInterface::DAS_PIXEL_FORMAT_HSV:
            cv::cvtColor(src, bgr, cv::COLOR_HSV2BGR);
            break;
        default:
            break;
        }
        return bgr;
    }

    auto CaptureViaBackend(Das::ExportInterface::IDasImage* p_image)
        -> std::shared_ptr<DebugImageSnapshot>
    {
        Das::DasPtr<Das::Core::OcvWrapper::IImageBackend> backend;
        const auto qi_result = p_image->QueryInterface(
            DasIidOf<Das::Core::OcvWrapper::IImageBackend>(),
            backend.PutVoid());
        if (qi_result < 0 || !backend)
        {
            return nullptr;
        }

        auto& mat = backend->GetCpuMat();
        if (mat.empty())
        {
            return nullptr;
        }

        auto snapshot = std::make_shared<DebugImageSnapshot>();
        snapshot->pixel_format = backend->GetPixelFormatValue();
        snapshot->bgr_image = ConvertToBgr(mat.clone(), snapshot->pixel_format);
        snapshot->available = !snapshot->bgr_image.empty();
        snapshot->image_status =
            snapshot->available ? "available" : "not_available";
        return snapshot;
    }

    auto CaptureViaPublicAbi(Das::ExportInterface::IDasImage* p_image)
        -> std::shared_ptr<DebugImageSnapshot>
    {
        Das::ExportInterface::DasSize             size{};
        int32_t                                   channels = 0;
        Das::ExportInterface::DasImagePixelFormat format{};
        if (p_image->GetSize(&size) < 0
            || p_image->GetChannelCount(&channels) < 0
            || p_image->GetPixelFormat(&format) < 0 || size.width <= 0
            || size.height <= 0 || channels <= 0 || channels > 4)
        {
            return nullptr;
        }

        const auto expected_channels = ExpectedChannelCount(format);
        if (expected_channels == 0 || channels != expected_channels)
        {
            return nullptr;
        }

        Das::DasPtr<Das::ExportInterface::IDasBinaryBuffer> buffer;
        if (p_image->GetBinaryBuffer(buffer.Put()) < 0 || !buffer)
        {
            return nullptr;
        }

        unsigned char* p_data = nullptr;
        uint64_t       data_size = 0;
        if (buffer->GetData(&p_data) < 0 || buffer->GetSize(&data_size) < 0
            || !p_data)
        {
            return nullptr;
        }

        const auto expected_size =
            static_cast<uint64_t>(size.width) * size.height * channels;
        if (data_size < expected_size)
        {
            return nullptr;
        }

        cv::Mat raw{size.height, size.width, CV_8UC(channels), p_data};
        auto    snapshot = std::make_shared<DebugImageSnapshot>();
        snapshot->pixel_format = format;
        snapshot->bgr_image = ConvertToBgr(raw.clone(), format);
        snapshot->available = !snapshot->bgr_image.empty();
        snapshot->image_status =
            snapshot->available ? "available" : "not_available";
        return snapshot;
    }

    void DrawBoxes(cv::Mat& image, const std::vector<DebugDrawBox>& annotations)
    {
        for (const auto& annotation : annotations)
        {
            const auto& rect = annotation.rect;
            if (rect.width <= 0 || rect.height <= 0)
            {
                continue;
            }

            cv::rectangle(
                image,
                cv::Rect{rect.x, rect.y, rect.width, rect.height},
                ColorOf(annotation.color),
                2);
            if (!annotation.label.empty())
            {
                cv::putText(
                    image,
                    annotation.label,
                    cv::Point{rect.x, std::max(0, rect.y - 4)},
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.45,
                    ColorOf(annotation.color),
                    1);
            }
        }
    }

    void DrawPoints(
        cv::Mat&                           image,
        const std::vector<DebugDrawPoint>& annotations)
    {
        for (const auto& annotation : annotations)
        {
            if (annotation.radius <= 0)
            {
                continue;
            }

            const auto color = ColorOf(annotation.color);
            const auto point = cv::Point{annotation.x, annotation.y};
            cv::circle(image, point, annotation.radius, color, 2);
            cv::line(
                image,
                cv::Point{annotation.x - annotation.radius, annotation.y},
                cv::Point{annotation.x + annotation.radius, annotation.y},
                color,
                1);
            cv::line(
                image,
                cv::Point{annotation.x, annotation.y - annotation.radius},
                cv::Point{annotation.x, annotation.y + annotation.radius},
                color,
                1);
            if (!annotation.label.empty())
            {
                cv::putText(
                    image,
                    annotation.label,
                    cv::Point{
                        annotation.x + annotation.radius + 3,
                        std::max(0, annotation.y - annotation.radius - 3)},
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.45,
                    color,
                    1);
            }
        }
    }

    void DrawLines(
        cv::Mat&                          image,
        const std::vector<DebugDrawLine>& annotations)
    {
        for (const auto& annotation : annotations)
        {
            if (annotation.thickness <= 0)
            {
                continue;
            }

            const auto color = ColorOf(annotation.color);
            const auto from = cv::Point{annotation.from_x, annotation.from_y};
            const auto to = cv::Point{annotation.to_x, annotation.to_y};
            cv::line(image, from, to, color, annotation.thickness);
            cv::circle(image, from, annotation.thickness + 2, color, -1);
            cv::circle(image, to, annotation.thickness + 2, color, -1);
            if (!annotation.label.empty())
            {
                cv::putText(
                    image,
                    annotation.label,
                    cv::Point{
                        std::min(annotation.from_x, annotation.to_x),
                        std::max(
                            0,
                            std::min(annotation.from_y, annotation.to_y) - 4)},
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.45,
                    color,
                    1);
            }
        }
    }

    void DrawAnnotations(
        cv::Mat&                     image,
        const DebugImageAnnotations& annotations)
    {
        DrawBoxes(image, annotations.boxes);
        DrawPoints(image, annotations.points);
        DrawLines(image, annotations.lines);
    }

    auto WriteImageFile(const std::filesystem::path& path, const cv::Mat& image)
        -> DasResult
    {
        const auto parent = path.parent_path();
        if (!parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                DAS_CORE_LOG_ERROR(
                    "Debug image directory creation failed for {}: {}",
                    DAS::Utils::U8AsString(parent.u8string()),
                    ec.message());
                return DAS_E_INVALID_PATH;
            }
        }

        if (!cv::imwrite(path.string(), image))
        {
            DAS_CORE_LOG_ERROR(
                "Debug image write failed for {}",
                DAS::Utils::U8AsString(path.u8string()));
            return DAS_E_INVALID_FILE;
        }

        return DAS_S_OK;
    }

    auto ProcessImageJob(const ImageJob& job) -> DasResult
    {
        try
        {
            auto result = WriteImageFile(job.original_path, job.original);
            if (DAS::IsFailed(result))
            {
                return result;
            }

            result = WriteImageFile(job.annotated_path, job.annotated);
            if (DAS::IsFailed(result))
            {
                return result;
            }

            return DAS_S_OK;
        }
        catch (const std::bad_alloc& ex)
        {
            DAS_CORE_LOG_ERROR(
                "Debug image worker out of memory: {}",
                ex.what());
            return DAS_E_OUT_OF_MEMORY;
        }
        catch (const cv::Exception& ex)
        {
            DAS_CORE_LOG_ERROR(
                "Debug image worker OpenCV error: {}",
                ex.what());
            return DAS_E_OPENCV_ERROR;
        }
        catch (const std::exception& ex)
        {
            DAS_CORE_LOG_ERROR("Debug image worker error: {}", ex.what());
            return DAS_E_FAIL;
        }
        catch (...)
        {
            DAS_CORE_LOG_ERROR("Debug image worker unknown error");
            return DAS_E_FAIL;
        }
    }

    void WorkerLoop()
    {
        auto& state = WorkerState();
        for (;;)
        {
            ImageJob job{};
            {
                std::unique_lock lock{state.mutex};
                state.cv.wait(
                    lock,
                    [&state]()
                    { return state.stopping || !state.queue.empty(); });
                if (state.stopping && state.queue.empty())
                {
                    return;
                }
                job = std::move(state.queue.front());
                state.queue.pop_front();
                state.active = true;
            }

            const auto job_result = ProcessImageJob(job);

            {
                std::lock_guard lock{state.mutex};
                if (DAS::IsFailed(job_result)
                    && !DAS::IsFailed(state.first_error))
                {
                    state.first_error = job_result;
                }
                state.active = false;
            }
            state.cv.notify_all();
        }
    }

    void EnsureWorkerRunning()
    {
        auto& state = WorkerState();
        if (state.running)
        {
            return;
        }
        state.stopping = false;
        state.worker = std::thread([]() { WorkerLoop(); });
        state.running = true;
    }

    void EnqueueImageJob(ImageJob job)
    {
        auto& state = WorkerState();
        {
            std::lock_guard lock{state.mutex};
            EnsureWorkerRunning();
            state.queue.emplace_back(std::move(job));
        }
        state.cv.notify_all();
    }

} // namespace

std::shared_ptr<DebugImageSnapshot> CaptureImageSnapshot(
    Das::ExportInterface::IDasImage* p_image)
{
    if (!p_image)
    {
        return std::make_shared<DebugImageSnapshot>();
    }

    try
    {
        if (auto snapshot = CaptureViaBackend(p_image))
        {
            return snapshot;
        }
        if (auto snapshot = CaptureViaPublicAbi(p_image))
        {
            return snapshot;
        }
    }
    catch (const cv::Exception& ex)
    {
        DAS_CORE_LOG_ERROR("Debug image capture OpenCV error: {}", ex.what());
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_ERROR("Debug image capture error: {}", ex.what());
    }
    return std::make_shared<DebugImageSnapshot>();
}

DebugImageWriteResult SaveOriginalAndAnnotated(
    const std::string&                        step_name,
    std::shared_ptr<const DebugImageSnapshot> snapshot,
    const std::vector<DebugDrawBox>&          annotations)
{
    DebugImageAnnotations all_annotations{};
    all_annotations.boxes = annotations;
    return SaveOriginalAndAnnotated(
        step_name,
        std::move(snapshot),
        all_annotations);
}

DebugImageWriteResult SaveOriginalAndAnnotated(
    const std::string&                        step_name,
    std::shared_ptr<const DebugImageSnapshot> snapshot,
    const DebugImageAnnotations&              annotations)
{
    DebugImageWriteResult result{};
    if (!snapshot || !snapshot->available || snapshot->bgr_image.empty())
    {
        return result;
    }

    const auto safe_step = SanitizeStepName(step_name);
    const auto timestamp = TimestampForFilename();
    result.original_image_filename = timestamp + "_" + safe_step + ".png";
    result.image_filename = "annotated_" + timestamp + "_" + safe_step + ".png";
    result.image_status = "available";

    auto annotated = snapshot->bgr_image.clone();
    DrawAnnotations(annotated, annotations);

    const auto img_dir = DebugRuntime::DebugDir() / "img";
    EnqueueImageJob(
        ImageJob{
            snapshot->bgr_image.clone(),
            std::move(annotated),
            img_dir / result.original_image_filename,
            img_dir / result.image_filename});

    return result;
}

std::string BuildImageJson(const DebugImageWriteResult& result)
{
    auto obj = DAS::Utils::MakeYyjsonObject();
    (*obj.as_object())[std::string_view("image_status")] = std::make_pair(
        std::string_view(result.image_status),
        yyjson::copy_string);
    (*obj.as_object())[std::string_view("original_image_filename")] =
        std::make_pair(
            std::string_view(result.original_image_filename),
            yyjson::copy_string);
    (*obj.as_object())[std::string_view("image_filename")] = std::make_pair(
        std::string_view(result.image_filename),
        yyjson::copy_string);
    auto serialized = DAS::Utils::SerializeYyjsonValue(obj);
    return serialized.value_or("{}");
}

DasResult DrainImageJobs()
{
    auto&            state = WorkerState();
    std::unique_lock lock{state.mutex};
    state.cv.wait(
        lock,
        [&state]() { return state.queue.empty() && !state.active; });
    const auto result = state.first_error;
    state.first_error = DAS_S_OK;
    return result;
}

void ShutdownImageWorker()
{
    auto& state = WorkerState();
    {
        std::lock_guard lock{state.mutex};
        if (!state.running)
        {
            return;
        }
        state.stopping = true;
    }
    state.cv.notify_all();
    if (state.worker.joinable())
    {
        state.worker.join();
    }
    {
        std::lock_guard lock{state.mutex};
        state.running = false;
        state.stopping = false;
    }
}

bool IsImageWorkerRunningForTest()
{
    auto&           state = WorkerState();
    std::lock_guard lock{state.mutex};
    return state.running;
}

DAS_CORE_DEBUG_NS_END

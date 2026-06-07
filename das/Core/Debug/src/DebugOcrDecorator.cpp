#include <das/Core/Debug/DebugDecorators.h>

#include <das/Core/Debug/DebugEvent.h>
#include <das/Core/Debug/DebugImageAnnotator.h>
#include <das/Core/Debug/DebugRuntime.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasOcr.Implements.hpp>

#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

DAS_CORE_DEBUG_NS_BEGIN
namespace
{
    using Clock = std::chrono::steady_clock;

    struct OcrCharInfo
    {
        Das::ExportInterface::DasRect rect{};
        double                        score{};
        bool                          has_score{false};
    };

    struct OcrLineInfo
    {
        std::string                   text;
        Das::ExportInterface::DasRect box{};
        double                        score{};
        uint32_t                      char_count{};
        std::vector<OcrCharInfo>      chars;
    };

    auto ElapsedMs(Clock::time_point start) -> double
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start)
            .count();
    }

    auto SerializeJson(yyjson::value value) -> std::string
    {
        auto serialized = DAS::Utils::SerializeYyjsonValue(value);
        return serialized.value_or("{}");
    }

    auto RectJson(const Das::ExportInterface::DasRect& rect) -> yyjson::value
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("x")] =
            static_cast<int64_t>(rect.x);
        (*obj.as_object())[std::string_view("y")] =
            static_cast<int64_t>(rect.y);
        (*obj.as_object())[std::string_view("width")] =
            static_cast<int64_t>(rect.width);
        (*obj.as_object())[std::string_view("height")] =
            static_cast<int64_t>(rect.height);
        return obj;
    }

    void AddImageFields(
        yyjson::value&               obj,
        const DebugImageWriteResult& image_result)
    {
        const auto image_json = BuildImageJson(image_result);
        auto       parsed = DAS::Utils::ParseYyjsonFromString(image_json);
        if (!parsed || !parsed->is_object())
        {
            return;
        }

        auto       fields = parsed->as_object();
        const auto status =
            (*fields)[std::string_view("image_status")].as_string();
        const auto original =
            (*fields)[std::string_view("original_image_filename")].as_string();
        const auto annotated =
            (*fields)[std::string_view("image_filename")].as_string();

        (*obj.as_object())[std::string_view("image_status")] =
            status ? std::string{*status} : std::string{};
        (*obj.as_object())[std::string_view("original_image_filename")] =
            original ? std::string{*original} : std::string{};
        (*obj.as_object())[std::string_view("image_filename")] =
            annotated ? std::string{*annotated} : std::string{};
    }

    auto ReadText(Das::ExportInterface::IDasOcrResult* result) -> std::string
    {
        if (!result)
        {
            return {};
        }

        Das::DasPtr<IDasReadOnlyString> text;
        if (result->GetText(text.Put()) != DAS_S_OK || !text)
        {
            return {};
        }

        const char* raw = nullptr;
        if (text->GetUtf8(&raw) != DAS_S_OK || !raw)
        {
            return {};
        }
        return raw;
    }

    auto ReadOcrLine(Das::ExportInterface::IDasOcrResult* result) -> OcrLineInfo
    {
        OcrLineInfo line{};
        if (!result)
        {
            return line;
        }

        line.text = ReadText(result);
        static_cast<void>(result->GetBox(&line.box));
        static_cast<void>(result->GetScore(&line.score));
        static_cast<void>(result->GetCharCount(&line.char_count));
        line.chars.reserve(line.char_count);

        for (uint32_t i = 0; i < line.char_count; ++i)
        {
            OcrCharInfo ch{};
            if (result->GetCharBox(i, &ch.rect) != DAS_S_OK)
            {
                continue;
            }
            if (result->GetCharScore(i, &ch.score) == DAS_S_OK)
            {
                ch.has_score = true;
            }
            line.chars.push_back(ch);
        }

        return line;
    }

    auto CollectOcrLines(Das::ExportInterface::IDasOcrResultVector* results)
        -> std::vector<OcrLineInfo>
    {
        std::vector<OcrLineInfo> lines;
        if (!results)
        {
            return lines;
        }

        uint32_t count = 0;
        if (results->GetCount(&count) != DAS_S_OK)
        {
            return lines;
        }

        lines.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            Das::DasPtr<Das::ExportInterface::IDasOcrResult> line;
            if (results->GetAt(i, line.Put()) == DAS_S_OK)
            {
                lines.push_back(ReadOcrLine(line.Get()));
            }
        }
        return lines;
    }

    auto OcrAnnotations(const std::vector<OcrLineInfo>& lines)
        -> std::vector<DebugDrawBox>
    {
        std::vector<DebugDrawBox> boxes;
        for (const auto& line : lines)
        {
            boxes.push_back(
                DebugDrawBox{line.box, DebugAnnotationColor::Green, "line"});
            for (const auto& ch : line.chars)
            {
                boxes.push_back(
                    DebugDrawBox{ch.rect, DebugAnnotationColor::Yellow, ""});
            }
        }
        return boxes;
    }

    auto CharJson(const OcrCharInfo& ch) -> yyjson::value
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("box")] = RectJson(ch.rect);
        if (ch.has_score)
        {
            (*obj.as_object())[std::string_view("score")] = ch.score;
        }
        return obj;
    }

    auto LineJson(const OcrLineInfo& line) -> yyjson::value
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("text")] =
            std::make_pair(std::string_view(line.text), yyjson::copy_string);
        (*obj.as_object())[std::string_view("box")] = RectJson(line.box);
        (*obj.as_object())[std::string_view("score")] = line.score;
        (*obj.as_object())[std::string_view("char_count")] =
            static_cast<uint64_t>(line.char_count);

        auto chars = DAS::Utils::MakeYyjsonArray();
        for (const auto& ch : line.chars)
        {
            (*chars.as_array()).emplace_back(CharJson(ch));
        }
        (*obj.as_object())[std::string_view("chars")] = std::move(chars);
        return obj;
    }

    auto LinesJsonArray(const std::vector<OcrLineInfo>& lines) -> yyjson::value
    {
        auto arr = DAS::Utils::MakeYyjsonArray();
        for (const auto& line : lines)
        {
            (*arr.as_array()).emplace_back(LineJson(line));
        }
        return arr;
    }

    auto CommonResultJson(
        DasResult                    result,
        const DebugImageWriteResult& image_result) -> yyjson::value
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("das_result")] =
            static_cast<int64_t>(result);
        (*obj.as_object())[std::string_view("success")] =
            !DAS::IsFailed(result);
        AddImageFields(obj, image_result);
        return obj;
    }

    void SubmitOcrEvent(
        std::string                  params_json,
        std::string                  result_json,
        const DebugImageWriteResult& image_result,
        double                       elapsed_ms)
    {
        auto event = MakeDebugEvent(
            "ocr_recognize",
            std::move(params_json),
            std::move(result_json));
        event.elapsed_ms = elapsed_ms;
        event.image_filename = image_result.image_filename;
        static_cast<void>(DebugRuntime::SubmitEvent(event));
    }

} // namespace

template <>
class DebugDecorator<Das::ExportInterface::IDasOcr> final
    : public Das::ExportInterface::DasOcrImplBase<
          DebugDecorator<Das::ExportInterface::IDasOcr>>
{
public:
    DebugDecorator(
        Das::ExportInterface::IDasOcr* inner,
        std::string                    operation_name)
        : inner_(Das::DasPtr<Das::ExportInterface::IDasOcr>::Attach(inner)),
          operation_name_(std::move(operation_name))
    {
    }

    DAS_IMPL Recognize(
        Das::ExportInterface::IDasImage*            p_image,
        Das::ExportInterface::IDasOcrResultVector** pp_results) override
    {
        if (!inner_)
        {
            return DAS_E_INVALID_POINTER;
        }

        const auto start = Clock::now();
        const auto result = inner_->Recognize(p_image, pp_results);
        const auto elapsed = ElapsedMs(start);

        std::vector<OcrLineInfo> lines;
        if (!DAS::IsFailed(result) && pp_results && *pp_results)
        {
            lines = CollectOcrLines(*pp_results);
        }

        auto image_result = SaveOriginalAndAnnotated(
            "ocr_recognize",
            CaptureImageSnapshot(p_image),
            OcrAnnotations(lines));

        auto params = DAS::Utils::MakeYyjsonObject();
        (*params.as_object())[std::string_view("operation_name")] =
            operation_name_;
        (*params.as_object())[std::string_view("method")] = "Recognize";

        auto result_json = CommonResultJson(result, image_result);
        (*result_json.as_object())[std::string_view("line_count")] =
            static_cast<uint64_t>(lines.size());
        (*result_json.as_object())[std::string_view("lines")] =
            LinesJsonArray(lines);

        SubmitOcrEvent(
            SerializeJson(std::move(params)),
            SerializeJson(std::move(result_json)),
            image_result,
            elapsed);
        return result;
    }

private:
    Das::DasPtr<Das::ExportInterface::IDasOcr> inner_;
    std::string                                operation_name_;
};

Das::ExportInterface::IDasOcr* MaybeDecorateOcrRaw(
    Das::ExportInterface::IDasOcr* p_raw,
    const char*                    p_operation_name)
{
    if (!p_raw || !DebugRuntime::IsEnabled())
    {
        return p_raw;
    }

    const std::string operation_name =
        p_operation_name ? p_operation_name : "ocr";
    return DebugDecorator<Das::ExportInterface::IDasOcr>::MakeRaw(
        p_raw,
        operation_name);
}

DAS_CORE_DEBUG_NS_END

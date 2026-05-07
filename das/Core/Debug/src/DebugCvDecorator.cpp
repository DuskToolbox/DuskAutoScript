#include <das/Core/Debug/DebugDecorators.h>

#include <das/Core/Debug/DebugEvent.h>
#include <das/Core/Debug/DebugImageAnnotator.h>
#include <das/Core/Debug/DebugRuntime.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasCv.Implements.hpp>

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

    struct TemplateMatchInfo
    {
        Das::ExportInterface::DasRect rect{};
        double                        score{};
        double                        raw_score{};
        bool                          valid{false};
    };

    auto ElapsedMs(Clock::time_point start) -> double
    {
        return std::chrono::duration<double, std::milli>(
                   Clock::now() - start)
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

    auto SizeJson(Das::ExportInterface::IDasImage* image) -> yyjson::value
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        Das::ExportInterface::DasSize size{};
        if (image && image->GetSize(&size) == DAS_S_OK)
        {
            (*obj.as_object())[std::string_view("width")] =
                static_cast<int64_t>(size.width);
            (*obj.as_object())[std::string_view("height")] =
                static_cast<int64_t>(size.height);
        }
        return obj;
    }

    auto ColorValueJson(const Das::ExportInterface::DasColorValue& value)
        -> yyjson::value
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("c1")] =
            static_cast<uint64_t>(value.c1);
        (*obj.as_object())[std::string_view("c2")] =
            static_cast<uint64_t>(value.c2);
        (*obj.as_object())[std::string_view("c3")] =
            static_cast<uint64_t>(value.c3);
        (*obj.as_object())[std::string_view("c4")] =
            static_cast<uint64_t>(value.c4);
        return obj;
    }

    void AddImageFields(
        yyjson::value&                    obj,
        const DebugImageWriteResult&      image_result)
    {
        const auto image_json = BuildImageJson(image_result);
        auto       parsed = DAS::Utils::ParseYyjsonFromString(image_json);
        if (!parsed || !parsed->is_object())
        {
            return;
        }

        auto fields = parsed->as_object();
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

    auto ReadTemplateMatch(
        Das::ExportInterface::IDasTemplateMatchResult* result)
        -> TemplateMatchInfo
    {
        TemplateMatchInfo info{};
        if (!result)
        {
            return info;
        }

        if (result->Getmatch_rect(&info.rect) != DAS_S_OK
            || result->Getscore(&info.score) != DAS_S_OK
            || result->Getraw_score(&info.raw_score) != DAS_S_OK)
        {
            return info;
        }

        info.valid = true;
        return info;
    }

    auto ScoreLabel(double score) -> std::string
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(3) << score;
        return stream.str();
    }

    auto MatchJson(const TemplateMatchInfo& info) -> yyjson::value
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("rect")] = RectJson(info.rect);
        (*obj.as_object())[std::string_view("score")] = info.score;
        (*obj.as_object())[std::string_view("raw_score")] = info.raw_score;
        return obj;
    }

    auto MatchAnnotations(const std::vector<TemplateMatchInfo>& matches)
        -> std::vector<DebugDrawBox>
    {
        std::vector<DebugDrawBox> boxes;
        boxes.reserve(matches.size());
        for (const auto& match : matches)
        {
            if (!match.valid)
            {
                continue;
            }

            boxes.push_back(DebugDrawBox{
                match.rect,
                DebugAnnotationColor::Green,
                ScoreLabel(match.score)});
        }
        return boxes;
    }

    auto MatchesJsonArray(const std::vector<TemplateMatchInfo>& matches)
        -> yyjson::value
    {
        auto arr = DAS::Utils::MakeYyjsonArray();
        for (const auto& match : matches)
        {
            if (match.valid)
            {
                (*arr.as_array()).emplace_back(MatchJson(match));
            }
        }
        return arr;
    }

    auto CommonCvParamsJson(
        const std::string& service_name,
        const char*        method_name) -> yyjson::value
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("service_name")] = service_name;
        (*obj.as_object())[std::string_view("method")] = method_name;
        return obj;
    }

    auto CommonResultJson(
        DasResult                       result,
        const DebugImageWriteResult&    image_result) -> yyjson::value
    {
        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("das_result")] =
            static_cast<int64_t>(result);
        (*obj.as_object())[std::string_view("success")] =
            !DAS::IsFailed(result);
        AddImageFields(obj, image_result);
        return obj;
    }

    void SubmitCvEvent(
        const char*                     event_type,
        std::string                     params_json,
        std::string                     result_json,
        const DebugImageWriteResult&    image_result,
        double                          elapsed_ms)
    {
        auto event = MakeDebugEvent(event_type, std::move(params_json),
                                    std::move(result_json));
        event.elapsed_ms = elapsed_ms;
        event.image_filename = image_result.image_filename;
        static_cast<void>(DebugRuntime::SubmitEvent(event));
    }

} // namespace

template <>
class DebugDecorator<Das::ExportInterface::IDasCv> final
    : public Das::ExportInterface::DasCvImplBase<
          DebugDecorator<Das::ExportInterface::IDasCv>>
{
public:
    DebugDecorator(
        Das::ExportInterface::IDasCv* inner,
        std::string                   service_name)
        : inner_(Das::DasPtr<Das::ExportInterface::IDasCv>::Attach(inner)),
          service_name_(std::move(service_name))
    {
    }

    DAS_IMPL TemplateMatchBest(
        Das::ExportInterface::IDasImage*                p_image,
        Das::ExportInterface::IDasImage*                p_template,
        Das::ExportInterface::DasTemplateMatchType      type,
        Das::ExportInterface::IDasTemplateMatchResult** pp_out_result)
        override
    {
        if (!inner_)
        {
            return DAS_E_INVALID_POINTER;
        }

        const auto start = Clock::now();
        const auto result =
            inner_->TemplateMatchBest(p_image, p_template, type, pp_out_result);
        const auto elapsed = ElapsedMs(start);

        std::vector<TemplateMatchInfo> matches;
        if (!DAS::IsFailed(result) && pp_out_result && *pp_out_result)
        {
            auto match = ReadTemplateMatch(*pp_out_result);
            if (match.valid)
            {
                matches.push_back(match);
            }
        }

        auto image_result = SaveOriginalAndAnnotated(
            "template_match_best",
            CaptureImageSnapshot(p_image),
            MatchAnnotations(matches));

        auto params = CommonCvParamsJson(service_name_, "TemplateMatchBest");
        (*params.as_object())[std::string_view("match_type")] =
            static_cast<int64_t>(type);

        auto result_json = CommonResultJson(result, image_result);
        (*result_json.as_object())[std::string_view("match_count")] =
            static_cast<uint64_t>(matches.size());
        (*result_json.as_object())[std::string_view("matches")] =
            MatchesJsonArray(matches);

        SubmitCvEvent(
            "template_match_best",
            SerializeJson(std::move(params)),
            SerializeJson(std::move(result_json)),
            image_result,
            elapsed);
        return result;
    }

    DAS_IMPL CreateMatchConfig(
        Das::ExportInterface::DasDetectorType     detector_type,
        Das::ExportInterface::DasMatcherType      matcher_type,
        Das::ExportInterface::DasMatchParams      params,
        Das::ExportInterface::IDasCvMatchConfig** pp_out_config) override
    {
        if (!inner_)
        {
            return DAS_E_INVALID_POINTER;
        }
        return inner_->CreateMatchConfig(
            detector_type,
            matcher_type,
            params,
            pp_out_config);
    }

    DAS_IMPL MatchFeatures(
        Das::ExportInterface::IDasImage*          p_query,
        Das::ExportInterface::IDasImage*          p_train,
        Das::ExportInterface::IDasCvMatchConfig*  p_config,
        Das::ExportInterface::IDasCvMatchResult** pp_out_result) override
    {
        if (!inner_)
        {
            return DAS_E_INVALID_POINTER;
        }
        return inner_->MatchFeatures(p_query, p_train, p_config, pp_out_result);
    }

    DAS_IMPL TemplateMatchAll(
        Das::ExportInterface::IDasImage*                 p_image,
        Das::ExportInterface::IDasImage*                 p_template,
        Das::ExportInterface::DasTemplateMatchType       type,
        double                                           threshold,
        int32_t                                          max_count,
        Das::ExportInterface::IDasTemplateMatchResults** pp_out_results)
        override
    {
        if (!inner_)
        {
            return DAS_E_INVALID_POINTER;
        }

        const auto start = Clock::now();
        const auto result = inner_->TemplateMatchAll(
            p_image,
            p_template,
            type,
            threshold,
            max_count,
            pp_out_results);
        const auto elapsed = ElapsedMs(start);

        uint32_t raw_match_count = 0;
        std::vector<TemplateMatchInfo> matches;
        if (!DAS::IsFailed(result) && pp_out_results && *pp_out_results)
        {
            uint32_t count = 0;
            static_cast<void>((*pp_out_results)->GetRawMatchCount(
                &raw_match_count));
            if ((*pp_out_results)->GetCount(&count) == DAS_S_OK)
            {
                matches.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    Das::DasPtr<
                        Das::ExportInterface::IDasTemplateMatchResult>
                        match;
                    if ((*pp_out_results)->GetAt(i, match.Put()) == DAS_S_OK)
                    {
                        auto info = ReadTemplateMatch(match.Get());
                        if (info.valid)
                        {
                            matches.push_back(info);
                        }
                    }
                }
            }
        }

        auto image_result = SaveOriginalAndAnnotated(
            "template_match_all",
            CaptureImageSnapshot(p_image),
            MatchAnnotations(matches));

        auto params = CommonCvParamsJson(service_name_, "TemplateMatchAll");
        (*params.as_object())[std::string_view("match_type")] =
            static_cast<int64_t>(type);
        (*params.as_object())[std::string_view("threshold")] = threshold;
        (*params.as_object())[std::string_view("max_count")] =
            static_cast<int64_t>(max_count);

        auto result_json = CommonResultJson(result, image_result);
        (*result_json.as_object())[std::string_view("match_count")] =
            static_cast<uint64_t>(matches.size());
        (*result_json.as_object())[std::string_view("raw_match_count")] =
            static_cast<uint64_t>(raw_match_count);
        (*result_json.as_object())[std::string_view("matches")] =
            MatchesJsonArray(matches);

        SubmitCvEvent(
            "template_match_all",
            SerializeJson(std::move(params)),
            SerializeJson(std::move(result_json)),
            image_result,
            elapsed);
        return result;
    }

    DAS_IMPL ConvertColor(
        Das::ExportInterface::IDasImage*          p_src,
        Das::ExportInterface::DasImagePixelFormat target_format,
        Das::ExportInterface::IDasImage**         pp_out_image) override
    {
        if (!inner_)
        {
            return DAS_E_INVALID_POINTER;
        }

        Das::ExportInterface::DasImagePixelFormat source_format{
            Das::ExportInterface::DAS_PIXEL_FORMAT_UNKNOWN};
        if (p_src)
        {
            static_cast<void>(p_src->GetPixelFormat(&source_format));
        }

        const auto start = Clock::now();
        const auto result =
            inner_->ConvertColor(p_src, target_format, pp_out_image);
        const auto elapsed = ElapsedMs(start);

        Das::ExportInterface::IDasImage* image_for_output =
            (!DAS::IsFailed(result) && pp_out_image && *pp_out_image)
                ? *pp_out_image
                : p_src;
        auto image_result = SaveOriginalAndAnnotated(
            "convert_color",
            CaptureImageSnapshot(image_for_output),
            std::vector<DebugDrawBox>{});

        auto params = CommonCvParamsJson(service_name_, "ConvertColor");
        (*params.as_object())[std::string_view("source_format")] =
            static_cast<int64_t>(source_format);
        (*params.as_object())[std::string_view("target_format")] =
            static_cast<int64_t>(target_format);

        auto result_json = CommonResultJson(result, image_result);
        (*result_json.as_object())[std::string_view("output_size")] =
            SizeJson(image_for_output);

        SubmitCvEvent(
            "convert_color",
            SerializeJson(std::move(params)),
            SerializeJson(std::move(result_json)),
            image_result,
            elapsed);
        return result;
    }

    DAS_IMPL ColorFilter(
        Das::ExportInterface::IDasImage*           p_src,
        const Das::ExportInterface::DasColorRange* p_range,
        Das::ExportInterface::IDasImage**          pp_out_mask) override
    {
        if (!inner_)
        {
            return DAS_E_INVALID_POINTER;
        }

        Das::ExportInterface::DasImagePixelFormat source_format{
            Das::ExportInterface::DAS_PIXEL_FORMAT_UNKNOWN};
        if (p_src)
        {
            static_cast<void>(p_src->GetPixelFormat(&source_format));
        }

        const auto start = Clock::now();
        const auto result = inner_->ColorFilter(p_src, p_range, pp_out_mask);
        const auto elapsed = ElapsedMs(start);

        Das::ExportInterface::IDasImage* image_for_output =
            (!DAS::IsFailed(result) && pp_out_mask && *pp_out_mask)
                ? *pp_out_mask
                : p_src;
        auto image_result = SaveOriginalAndAnnotated(
            "color_filter",
            CaptureImageSnapshot(image_for_output),
            std::vector<DebugDrawBox>{});

        auto params = CommonCvParamsJson(service_name_, "ColorFilter");
        (*params.as_object())[std::string_view("source_format")] =
            static_cast<int64_t>(source_format);
        if (p_range)
        {
            (*params.as_object())[std::string_view("lower")] =
                ColorValueJson(p_range->lower);
            (*params.as_object())[std::string_view("upper")] =
                ColorValueJson(p_range->upper);
        }

        auto result_json = CommonResultJson(result, image_result);
        (*result_json.as_object())[std::string_view("mask_size")] =
            SizeJson(image_for_output);

        SubmitCvEvent(
            "color_filter",
            SerializeJson(std::move(params)),
            SerializeJson(std::move(result_json)),
            image_result,
            elapsed);
        return result;
    }

private:
    Das::DasPtr<Das::ExportInterface::IDasCv> inner_;
    std::string                               service_name_;
};

Das::ExportInterface::IDasCv* MaybeDecorateCvRaw(
    Das::ExportInterface::IDasCv* p_raw,
    const char*                   p_service_name)
{
    if (!p_raw || !DebugRuntime::IsEnabled())
    {
        return p_raw;
    }

    const std::string service_name =
        p_service_name ? p_service_name : "cv";
    return DebugDecorator<Das::ExportInterface::IDasCv>::MakeRaw(
        p_raw,
        service_name);
}

DAS_CORE_DEBUG_NS_END

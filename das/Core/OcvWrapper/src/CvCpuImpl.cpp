#include "CvCpuImpl.h"
#include "CpuImageImpl.h"
#include "DescriptorMatcherFactory.h"
#include "FeatureDetectorFactory.h"
#include "IDasImageImpl.h"
#include "IDasMatchResultImpl.h"
#include "IDasTemplateMatchResultImpl.h"
#include "IDasTemplateMatchResultsImpl.h"
#include "IImageBackend.h"
#include "IMatchConfigImpl.h"
#include <das/Core/OcvWrapper/Config.h>

#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <das/Utils/Timer.hpp>
#include <das/_autogen/idl/abi/DasCV.h>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

DAS_DISABLE_WARNING_END

#include <algorithm>
#include <cmath>
#include <vector>

DAS_CORE_OCVWRAPPER_NS_BEGIN

// ==================== Internal helpers ====================

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto GetImageBackend(ExportInterface::IDasImage* p_image)
    -> DAS::Utils::Expected<DasPtr<IImageBackend>>
{
    DasPtr<IImageBackend> p_result{};

    if (const auto qi_result = p_image->QueryInterface(
            DasIidOf<Das::Core::OcvWrapper::IImageBackend>(),
            p_result.PutVoid());
        IsFailed(qi_result))
    {
        DAS_CORE_LOG_ERROR(
            "Can not find interface IImageBackend in IDasImage object. "
            "Pointer = {}.",
            Utils::VoidP(p_image));
        return tl::make_unexpected(qi_result);
    }

    return p_result;
}

/// @brief Internal scan candidate for TemplateMatchAll
struct ScanCandidate
{
    float score{};
    float raw_score{};
    int   x{};
    int   y{};
};

/// @brief Compute IoU between two scan candidates
auto ComputeIoU(
    const ScanCandidate& a,
    const ScanCandidate& b,
    int                  tmpl_w,
    int                  tmpl_h) -> double
{
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + tmpl_w, b.x + tmpl_w);
    const int y2 = std::min(a.y + tmpl_h, b.y + tmpl_h);

    const int inter_w = std::max(0, x2 - x1);
    const int inter_h = std::max(0, y2 - y1);
    const int inter_area = inter_w * inter_h;

    const int area = tmpl_w * tmpl_h;
    const int union_area = area + area - inter_area;

    if (union_area <= 0)
    {
        return 0.0;
    }
    return static_cast<double>(inter_area) / static_cast<double>(union_area);
}

/// @brief Global NMS: sort by score descending, suppress overlapping candidates
auto ApplyNms(
    std::vector<ScanCandidate> candidates,
    int                        tmpl_w,
    int                        tmpl_h,
    double                     iou_threshold) -> std::vector<ScanCandidate>
{
    if (candidates.empty())
    {
        return {};
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const ScanCandidate& a, const ScanCandidate& b)
        { return a.score > b.score; });

    std::vector<ScanCandidate> result;
    result.reserve(candidates.size());
    std::vector<bool> suppressed(candidates.size(), false);

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (suppressed[i])
        {
            continue;
        }
        result.push_back(candidates[i]);
        for (size_t j = i + 1; j < candidates.size(); ++j)
        {
            if (suppressed[j])
            {
                continue;
            }
            if (ComputeIoU(candidates[i], candidates[j], tmpl_w, tmpl_h)
                > iou_threshold)
            {
                suppressed[j] = true;
            }
        }
    }

    result.shrink_to_fit();
    return result;
}

/// @brief Unify template match scores to 0..1 (higher is better)
auto UnifyScore(float raw_score, ExportInterface::DasTemplateMatchType type)
    -> float
{
    if (type == ExportInterface::DAS_TEMPLATE_MATCH_TYPE_SQDIFF_NORMED)
    {
        return 1.0f - raw_score;
    }
    return raw_score;
}

DAS_NS_ANONYMOUS_DETAILS_END

// ==================== TemplateMatchBest ====================

DasResult CvCpuImpl::TemplateMatchBest(
    ExportInterface::IDasImage*                p_image,
    ExportInterface::IDasImage*                p_template,
    ExportInterface::DasTemplateMatchType      type,
    ExportInterface::IDasTemplateMatchResult** pp_out_result)
{
    DAS_UTILS_CHECK_POINTER(pp_out_result)

    const auto expected_p_image = Details::GetImageBackend(p_image);
    if (!expected_p_image)
    {
        return expected_p_image.error();
    }

    const auto expected_p_template = Details::GetImageBackend(p_template);
    if (!expected_p_template)
    {
        return expected_p_template.error();
    }

    const auto& image_mat = expected_p_image.value()->GetCpuMat();
    const auto& template_mat = expected_p_template.value()->GetCpuMat();

    DAS::Utils::Timer timer{};
    timer.Begin();

    double    min_score = 0.0;
    double    max_score = 0.0;
    cv::Point min_location{};
    cv::Point max_location{};
    cv::Mat   output;
    cv::matchTemplate(
        image_mat,
        template_mat,
        output,
        DAS::Utils::ToUnderlying(type));
    cv::minMaxLoc(output, &min_score, &max_score, &min_location, &max_location);

    const auto cv_cost = timer.End();
    DAS_CORE_LOG_INFO(
        "Function matchTemplate and minMaxLoc cost {} ms.",
        cv_cost);

    if (std::isnan(max_score) || std::isinf(max_score))
    {
        max_score = 0;
    }

    if (std::isnan(min_score) || std::isinf(min_score))
    {
        min_score = 0;
    }

    cv::Point matched_location{};
    double    score{};
    if (type == ExportInterface::DAS_TEMPLATE_MATCH_TYPE_SQDIFF_NORMED)
    {
        matched_location = min_location;
        score = 1.0 - min_score;
    }
    else
    {
        matched_location = max_location;
        score = max_score;
    }

    *pp_out_result = IDasTemplateMatchResultImpl::MakeRaw(
        score,
        ExportInterface::DasRect{
            matched_location.x,
            matched_location.y,
            template_mat.cols,
            template_mat.rows});
    return DAS_S_OK;
}

// ==================== TemplateMatchAll ====================

DasResult CvCpuImpl::TemplateMatchAll(
    ExportInterface::IDasImage*                 p_image,
    ExportInterface::IDasImage*                 p_template,
    ExportInterface::DasTemplateMatchType       type,
    double                                      threshold,
    int32_t                                     max_count,
    ExportInterface::IDasTemplateMatchResults** pp_out_results)
{
    DAS_UTILS_CHECK_POINTER(pp_out_results)

    const auto expected_p_image = Details::GetImageBackend(p_image);
    if (!expected_p_image)
    {
        return expected_p_image.error();
    }

    const auto expected_p_template = Details::GetImageBackend(p_template);
    if (!expected_p_template)
    {
        return expected_p_template.error();
    }

    const auto& image_mat = expected_p_image.value()->GetCpuMat();
    const auto& template_mat = expected_p_template.value()->GetCpuMat();

    if (template_mat.rows > image_mat.rows
        || template_mat.cols > image_mat.cols)
    {
        return DAS_E_INVALID_SIZE;
    }

    const int tmpl_w = template_mat.cols;
    const int tmpl_h = template_mat.rows;

    // Run matchTemplate to get the score map
    cv::Mat result_mat;
    cv::matchTemplate(
        image_mat,
        template_mat,
        result_mat,
        DAS::Utils::ToUnderlying(type));

    // Step 1: Threshold filtering — collect all candidates with score >=
    // threshold
    std::vector<Details::ScanCandidate> candidates;
    candidates.reserve(static_cast<size_t>(result_mat.rows * result_mat.cols));

    for (int y = 0; y < result_mat.rows; ++y)
    {
        const auto* row = result_mat.ptr<float>(y);
        for (int x = 0; x < result_mat.cols; ++x)
        {
            const float raw = row[x];
            const float unified = Details::UnifyScore(raw, type);

            if (unified >= threshold)
            {
                candidates.push_back({unified, raw, x, y});
            }
        }
    }

    // Step 2: Record raw_match_count (before NMS)
    const uint32_t raw_match_count = static_cast<uint32_t>(candidates.size());

    // Step 3: Keep internal scan trajectory (for debug purposes)
    // The candidates vector itself IS the raw scan trajectory.
    // We keep it alive in this local scope for potential debug access.

    // Step 4: Global NMS with default IoU threshold of 0.5
    constexpr double kIouThreshold = 0.5;
    auto             final_candidates =
        Details::ApplyNms(std::move(candidates), tmpl_w, tmpl_h, kIouThreshold);

    // Step 5: Sort by score descending (already done by ApplyNms)

    // Step 6: Apply max_count truncation
    if (max_count > 0
        && static_cast<int32_t>(final_candidates.size()) > max_count)
    {
        final_candidates.resize(static_cast<size_t>(max_count));
    }

    // Step 7: Create result collection
    auto* p_results =
        DAS::Core::OcvWrapper::IDasTemplateMatchResultsImpl::MakeRaw();
    p_results->SetRawMatchCount(raw_match_count);
    p_results->Reserve(final_candidates.size());

    for (const auto& cand : final_candidates)
    {
        const float raw_score = cand.raw_score;

        auto* p_result = IDasTemplateMatchResultImpl::MakeRaw(
            cand.score,
            ExportInterface::DasRect{cand.x, cand.y, tmpl_w, tmpl_h},
            raw_score);
        p_results->AddResult(p_result);
        p_result->Release();
    }

    *pp_out_results = p_results;
    return DAS_S_OK;
}

// ==================== CreateMatchConfig ====================

DasResult CvCpuImpl::CreateMatchConfig(
    ExportInterface::DasDetectorType     detector_type,
    ExportInterface::DasMatcherType      matcher_type,
    ExportInterface::DasMatchParams      params,
    ExportInterface::IDasCvMatchConfig** pp_out_config)
{
    DAS_UTILS_CHECK_POINTER(pp_out_config)

    if (params.ratio_threshold < 0.0f || params.ratio_threshold > 1.0f)
    {
        DAS_CORE_LOG_ERROR(
            "ratio_threshold out of range: {}",
            params.ratio_threshold);
        return DAS_E_INVALID_ARGUMENT;
    }

    if (params.max_keypoints == 0)
    {
        DAS_CORE_LOG_ERROR("max_keypoints is zero");
        return DAS_E_INVALID_ARGUMENT;
    }

    auto* p_config =
        IMatchConfigImpl::MakeRaw(detector_type, matcher_type, params);
    *pp_out_config = p_config;
    return DAS_S_OK;
}

// ==================== MatchFeatures ====================

DasResult CvCpuImpl::MatchFeatures(
    ExportInterface::IDasImage*          p_query,
    ExportInterface::IDasImage*          p_train,
    ExportInterface::IDasCvMatchConfig*  p_config,
    ExportInterface::IDasCvMatchResult** pp_out_result)
{
    DAS_UTILS_CHECK_POINTER(p_query)
    DAS_UTILS_CHECK_POINTER(p_train)
    DAS_UTILS_CHECK_POINTER(pp_out_result)

    if (!p_config)
    {
        return DAS_E_INVALID_POINTER;
    }

    Das::Utils::Timer timer{};
    timer.Begin();

    ExportInterface::DasDetectorType detector_type{};
    ExportInterface::DasMatcherType  matcher_type{};
    ExportInterface::DasMatchParams  params{};

    auto result = p_config->GetDetectorType(&detector_type);
    if (Das::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("Failed to get detector type");
        return result;
    }

    result = p_config->GetMatcherType(&matcher_type);
    if (Das::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("Failed to get matcher type");
        return result;
    }

    result = p_config->GetParams(&params);
    if (Das::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("Failed to get match params");
        return result;
    }

    const auto expected_query = Details::GetImageBackend(p_query);
    if (!expected_query)
    {
        return expected_query.error();
    }

    const auto expected_train = Details::GetImageBackend(p_train);
    if (!expected_train)
    {
        return expected_train.error();
    }

    const auto& query_mat = expected_query.value()->GetCpuMat();
    const auto& train_mat = expected_train.value()->GetCpuMat();

    auto detector =
        Details::CreateDetector(detector_type, params.max_keypoints);
    if (!detector)
    {
        DAS_CORE_LOG_ERROR(
            "Failed to create detector, type = {}",
            static_cast<int>(detector_type));
        return DAS_E_FAIL;
    }

    std::vector<cv::KeyPoint> query_keypoints, train_keypoints;
    cv::Mat                   query_descriptors, train_descriptors;

    detector->detectAndCompute(
        query_mat,
        cv::noArray(),
        query_keypoints,
        query_descriptors);
    detector->detectAndCompute(
        train_mat,
        cv::noArray(),
        train_keypoints,
        train_descriptors);

    if (query_descriptors.empty() || train_descriptors.empty())
    {
        auto* p_result = IDasMatchResultImpl::MakeRaw();
        *pp_out_result = p_result;
        return DAS_S_OK;
    }

    auto matcher = Details::CreateMatcher(matcher_type, detector_type);
    if (!matcher)
    {
        DAS_CORE_LOG_ERROR(
            "Failed to create matcher, type = {}",
            static_cast<int>(matcher_type));
        return DAS_E_FAIL;
    }

    std::vector<cv::DMatch> matches;
    matcher->match(query_descriptors, train_descriptors, matches);

    if (params.ratio_threshold > 0.0f && params.ratio_threshold < 1.0f)
    {
        std::vector<cv::DMatch> good_matches;
        good_matches.reserve(matches.size());
        for (const auto& match : matches)
        {
            if (match.distance < params.ratio_threshold)
            {
                good_matches.push_back(match);
            }
        }
        matches = std::move(good_matches);
    }

    auto* p_result = IDasMatchResultImpl::MakeRaw();
    p_result->Reserve(matches.size());

    for (const auto& match : matches)
    {
        auto das_match = Details::ToDasMatchedPoint(
            query_keypoints[match.queryIdx],
            train_keypoints[match.trainIdx],
            match.distance);
        p_result->AddMatch(das_match);
    }

    const auto elapsed = timer.End();
    DAS_CORE_LOG_INFO(
        "Feature matching completed in {} ms, {} matches",
        elapsed,
        matches.size());

    *pp_out_result = p_result;
    return DAS_S_OK;
}

// ==================== ConvertColor ====================

DasResult CvCpuImpl::ConvertColor(
    ExportInterface::IDasImage*          p_src,
    ExportInterface::DasImagePixelFormat target_format,
    ExportInterface::IDasImage**         pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_src)
    DAS_UTILS_CHECK_POINTER(pp_out_image)

    const auto expected_backend = Details::GetImageBackend(p_src);
    if (!expected_backend)
    {
        return expected_backend.error();
    }

    auto&       backend = *expected_backend.value();
    const auto  src_format = backend.GetPixelFormatValue();
    const auto& src_mat = backend.GetCpuMat();

    // Determine the color conversion code
    int conversion_code = -1;

    if (src_format == ExportInterface::DAS_PIXEL_FORMAT_BGR)
    {
        switch (target_format)
        {
        case ExportInterface::DAS_PIXEL_FORMAT_RGB:
            conversion_code = cv::COLOR_BGR2RGB;
            break;
        case ExportInterface::DAS_PIXEL_FORMAT_RGBA:
            conversion_code = cv::COLOR_BGR2RGBA;
            break;
        case ExportInterface::DAS_PIXEL_FORMAT_GRAY:
            conversion_code = cv::COLOR_BGR2GRAY;
            break;
        case ExportInterface::DAS_PIXEL_FORMAT_HSV:
            conversion_code = cv::COLOR_BGR2HSV;
            break;
        default:
            break;
        }
    }
    else if (src_format == ExportInterface::DAS_PIXEL_FORMAT_RGB)
    {
        switch (target_format)
        {
        case ExportInterface::DAS_PIXEL_FORMAT_BGR:
            conversion_code = cv::COLOR_RGB2BGR;
            break;
        case ExportInterface::DAS_PIXEL_FORMAT_RGBA:
            conversion_code = cv::COLOR_RGB2RGBA;
            break;
        case ExportInterface::DAS_PIXEL_FORMAT_GRAY:
            conversion_code = cv::COLOR_RGB2GRAY;
            break;
        case ExportInterface::DAS_PIXEL_FORMAT_HSV:
            conversion_code = cv::COLOR_RGB2HSV;
            break;
        default:
            break;
        }
    }
    else if (src_format == ExportInterface::DAS_PIXEL_FORMAT_RGBA)
    {
        switch (target_format)
        {
        case ExportInterface::DAS_PIXEL_FORMAT_BGR:
            conversion_code = cv::COLOR_RGBA2BGR;
            break;
        case ExportInterface::DAS_PIXEL_FORMAT_RGB:
            conversion_code = cv::COLOR_RGBA2RGB;
            break;
        case ExportInterface::DAS_PIXEL_FORMAT_GRAY:
            conversion_code = cv::COLOR_RGBA2GRAY;
            break;
        default:
            break;
        }
    }
    else if (src_format == ExportInterface::DAS_PIXEL_FORMAT_GRAY)
    {
        switch (target_format)
        {
        case ExportInterface::DAS_PIXEL_FORMAT_BGR:
            conversion_code = cv::COLOR_GRAY2BGR;
            break;
        case ExportInterface::DAS_PIXEL_FORMAT_RGB:
            conversion_code = cv::COLOR_GRAY2RGB;
            break;
        default:
            break;
        }
    }
    else if (src_format == ExportInterface::DAS_PIXEL_FORMAT_HSV)
    {
        switch (target_format)
        {
        case ExportInterface::DAS_PIXEL_FORMAT_BGR:
            conversion_code = cv::COLOR_HSV2BGR;
            break;
        case ExportInterface::DAS_PIXEL_FORMAT_RGB:
            conversion_code = cv::COLOR_HSV2RGB;
            break;
        default:
            break;
        }
    }

    if (conversion_code < 0)
    {
        DAS_CORE_LOG_ERROR(
            "Unsupported color conversion: src_format={}, target_format={}",
            static_cast<int>(src_format),
            static_cast<int>(target_format));
        return DAS_E_INVALID_ARGUMENT;
    }

    // Perform out-of-place color conversion
    cv::Mat dst_mat;
    cv::cvtColor(src_mat, dst_mat, conversion_code);

    // Create a new CpuImageImpl with the target format
    auto* p_result =
        CpuImageImpl::MakeFromCpuMat(std::move(dst_mat), target_format);
    p_result->AddRef();
    *pp_out_image = p_result;
    return DAS_S_OK;
}

// ==================== ColorFilter ====================

DasResult CvCpuImpl::ColorFilter(
    ExportInterface::IDasImage*           p_src,
    const ExportInterface::DasColorRange* p_range,
    ExportInterface::IDasImage**          pp_out_mask)
{
    DAS_UTILS_CHECK_POINTER(p_src)
    DAS_UTILS_CHECK_POINTER(p_range)
    DAS_UTILS_CHECK_POINTER(pp_out_mask)

    const auto expected_backend = Details::GetImageBackend(p_src);
    if (!expected_backend)
    {
        return expected_backend.error();
    }

    auto&       backend = *expected_backend.value();
    const auto  src_format = backend.GetPixelFormatValue();
    const auto& src_mat = backend.GetCpuMat();

    // Build cv::Scalar from DasColorRange based on pixel format semantics
    cv::Scalar lower{};
    cv::Scalar upper{};

    switch (src_format)
    {
    case ExportInterface::DAS_PIXEL_FORMAT_GRAY:
        lower = cv::Scalar(p_range->lower.c1);
        upper = cv::Scalar(p_range->upper.c1);
        break;
    case ExportInterface::DAS_PIXEL_FORMAT_BGR:
    case ExportInterface::DAS_PIXEL_FORMAT_RGB:
    case ExportInterface::DAS_PIXEL_FORMAT_HSV:
        lower =
            cv::Scalar(p_range->lower.c1, p_range->lower.c2, p_range->lower.c3);
        upper =
            cv::Scalar(p_range->upper.c1, p_range->upper.c2, p_range->upper.c3);
        break;
    case ExportInterface::DAS_PIXEL_FORMAT_RGBA:
        lower = cv::Scalar(
            p_range->lower.c1,
            p_range->lower.c2,
            p_range->lower.c3,
            p_range->lower.c4);
        upper = cv::Scalar(
            p_range->upper.c1,
            p_range->upper.c2,
            p_range->upper.c3,
            p_range->upper.c4);
        break;
    default:
        // Unknown format: attempt 3-channel interpretation
        lower =
            cv::Scalar(p_range->lower.c1, p_range->lower.c2, p_range->lower.c3);
        upper =
            cv::Scalar(p_range->upper.c1, p_range->upper.c2, p_range->upper.c3);
        break;
    }

    // Apply inRange to produce a binary mask
    cv::Mat mask_mat;
    cv::inRange(src_mat, lower, upper, mask_mat);

    // Create a new CpuImageImpl for the mask (GRAY format, single channel)
    auto* p_result = CpuImageImpl::MakeFromCpuMat(
        std::move(mask_mat),
        ExportInterface::DAS_PIXEL_FORMAT_GRAY);
    p_result->AddRef();
    *pp_out_mask = p_result;
    return DAS_S_OK;
}

DAS_CORE_OCVWRAPPER_NS_END

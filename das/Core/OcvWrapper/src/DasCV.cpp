#include "Config.h"
#include "DescriptorMatcherFactory.h"
#include "FeatureDetectorFactory.h"
#include "IDasImageImpl.h"
#include "IDasMatchResultImpl.h"
#include "IDasTemplateMatchResultImpl.h"
#include "IMatchConfigImpl.h"
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

DAS_CORE_OCVWRAPPER_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto GetDasImageImpl(ExportInterface::IDasImage* p_image)
    -> DAS::Utils::Expected<DasPtr<IDasImageImpl>>
{
    DasPtr<IDasImageImpl> p_result{};

    if (const auto qi_result = p_image->QueryInterface(
            DasIidOf<IDasImageImpl>(),
            p_result.PutVoid());
        IsFailed(qi_result))
    {
        DAS_CORE_LOG_ERROR(
            "Can not find interface Das::Core::OcvWrapper::IDasImageImpl in IDasImage object. Pointer = {}.",
            Utils::VoidP(p_image));
        return tl::make_unexpected(qi_result);
    }

    return p_result;
}

DAS_NS_ANONYMOUS_DETAILS_END

DAS_CORE_OCVWRAPPER_NS_END

DasResult TemplateMatchBest(
    Das::ExportInterface::IDasImage*                p_image,
    Das::ExportInterface::IDasImage*                p_template,
    Das::ExportInterface::DasTemplateMatchType      type,
    Das::ExportInterface::IDasTemplateMatchResult** pp_out_result)
{
    DAS_UTILS_CHECK_POINTER(pp_out_result)

    const auto expected_p_image =
        DAS::Core::OcvWrapper::Details::GetDasImageImpl(p_image);

    if (!expected_p_image)
    {
        return expected_p_image.error();
    }

    const auto expected_p_template =
        DAS::Core::OcvWrapper::Details::GetDasImageImpl(p_template);

    if (!expected_p_template)
    {
        return expected_p_template.error();
    }

    const auto& image_mat = expected_p_image.value()->GetImpl();
    const auto& template_mat = expected_p_template.value()->GetImpl();

    DAS::Utils::Timer timer{};
    timer.Begin();

    // 由于已经限制了type，可以使用的都是规范化后的值，所以输出应当出于0-1的区间
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

    cv::Point matched_location{};
    double    score{};
    if (type == Das::ExportInterface::DAS_TEMPLATE_MATCH_TYPE_SQDIFF_NORMED)
    {
        matched_location = min_location;
        score = 1 - min_score;
    }
    else
    {
        matched_location = max_location;
        score = max_score;
    }

    // Create result object and set values
    *pp_out_result =
        DAS::Core::OcvWrapper::IDasTemplateMatchResultImpl::MakeRaw(
            score,
            Das::ExportInterface::DasRect{
                matched_location.x,
                matched_location.y,
                template_mat.cols,
                template_mat.rows});
    return DAS_S_OK;
}

DasResult CreateMatchConfig(
    Das::ExportInterface::DasDetectorType     detector_type,
    Das::ExportInterface::DasMatcherType      matcher_type,
    Das::ExportInterface::DasMatchParams      params,
    Das::ExportInterface::IDasCvMatchConfig** pp_out_config)
{
    if (!pp_out_config)
        return DAS_E_INVALID_POINTER;

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

    auto* p_config = DAS::Core::OcvWrapper::IMatchConfigImpl::MakeRaw(
        detector_type,
        matcher_type,
        params);

    *pp_out_config = p_config;
    return DAS_S_OK;
}

DasResult MatchFeatures(
    Das::ExportInterface::IDasImage*          p_query,
    Das::ExportInterface::IDasImage*          p_train,
    Das::ExportInterface::IDasCvMatchConfig*  p_config,
    Das::ExportInterface::IDasCvMatchResult** pp_out_result)
{
    if (!p_query || !p_train || !pp_out_result)
        return DAS_E_INVALID_POINTER;

    if (!p_config)
        return DAS_E_INVALID_POINTER;

    Das::Utils::Timer timer{};
    timer.Begin();

    Das::ExportInterface::DasDetectorType detector_type{};
    Das::ExportInterface::DasMatcherType  matcher_type{};
    Das::ExportInterface::DasMatchParams  params{};

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

    const auto expected_query =
        DAS::Core::OcvWrapper::Details::GetDasImageImpl(p_query);
    if (!expected_query)
        return expected_query.error();

    const auto expected_train =
        DAS::Core::OcvWrapper::Details::GetDasImageImpl(p_train);
    if (!expected_train)
        return expected_train.error();

    const auto& query_mat = expected_query.value()->GetImpl();
    const auto& train_mat = expected_train.value()->GetImpl();

    auto detector = DAS::Core::OcvWrapper::Details::CreateDetector(
        detector_type,
        params.max_keypoints);
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
        auto* p_result = DAS::Core::OcvWrapper::IDasMatchResultImpl::MakeRaw();
        *pp_out_result = p_result;
        return DAS_S_OK;
    }

    auto matcher = DAS::Core::OcvWrapper::Details::CreateMatcher(
        matcher_type,
        detector_type);
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

    auto* p_result = DAS::Core::OcvWrapper::IDasMatchResultImpl::MakeRaw();
    p_result->Reserve(matches.size());

    for (const auto& match : matches)
    {
        auto das_match = DAS::Core::OcvWrapper::Details::ToDasMatchedPoint(
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

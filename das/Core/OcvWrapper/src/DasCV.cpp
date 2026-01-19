#include "Config.h"
#include "IDasImageImpl.h"
#include <das/Core/Logger/Logger.h>
#include <DAS/_autogen/idl/abi/DasCV.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <das/Utils/Timer.hpp>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>

DAS_DISABLE_WARNING_END

DAS_CORE_OCVWRAPPER_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto GetDasImageImpl(IDasImage* p_image)
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
    IDasImage*              p_image,
    IDasImage*              p_template,
    DasTemplateMatchType    type,
    DasTemplateMatchResult* p_out_result)
{
    DAS_UTILS_CHECK_POINTER(p_out_result)

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
    DAS_CORE_LOG_INFO("Function matchTemplate and minMaxLoc cost {} ms.", cv_cost);

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

    p_out_result->match_rect = DAS::DasRect{
        matched_location.x,
        matched_location.y,
        template_mat.cols,
        template_mat.rows};
    p_out_result->score = score;

    return DAS_S_OK;
}

DasRetTemplateMatchResult TemplateMatchBest(
    DasSwigImage         image,
    DasSwigImage         template_image,
    DasTemplateMatchType type)
{
    auto* const p_image = image.Get();
    auto* const p_template = template_image.Get();

    DasRetTemplateMatchResult result;
    result.error_code =
        TemplateMatchBest(p_image, p_template, type, &result.value);

    return result;
}
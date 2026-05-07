#include "IDasOcrResultImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

DAS_CORE_ORTWRAPPER_NS_BEGIN

IDasOcrResultImpl::IDasOcrResultImpl(
    std::string                                text,
    Das::ExportInterface::DasRect              box,
    double                                     score,
    std::vector<Das::ExportInterface::DasRect> char_boxes,
    std::vector<double>                        char_scores)
    : text_(std::move(text)), box_(box), score_(score),
      char_boxes_(std::move(char_boxes)), char_scores_(std::move(char_scores))
{
}

DasResult IDasOcrResultImpl::GetText(IDasReadOnlyString** pp_text)
{
    DAS_UTILS_CHECK_POINTER(pp_text);

    DasReadOnlyString str(text_.c_str());
    Das::Utils::SetResult(str, pp_text);
    return DAS_S_OK;
}

DasResult IDasOcrResultImpl::GetBox(Das::ExportInterface::DasRect* p_box)
{
    DAS_UTILS_CHECK_POINTER(p_box);

    *p_box = box_;
    return DAS_S_OK;
}

DasResult IDasOcrResultImpl::GetScore(double* p_score)
{
    DAS_UTILS_CHECK_POINTER(p_score);

    *p_score = score_;
    return DAS_S_OK;
}

DasResult IDasOcrResultImpl::GetCharCount(uint32_t* p_count)
{
    DAS_UTILS_CHECK_POINTER(p_count);

    *p_count = static_cast<uint32_t>(char_boxes_.size());
    return DAS_S_OK;
}

DasResult IDasOcrResultImpl::GetCharBox(
    uint32_t                       index,
    Das::ExportInterface::DasRect* p_box)
{
    DAS_UTILS_CHECK_POINTER(p_box);

    if (index >= static_cast<uint32_t>(char_boxes_.size()))
    {
        return DAS_E_OUT_OF_RANGE;
    }

    *p_box = char_boxes_[index];
    return DAS_S_OK;
}

DasResult IDasOcrResultImpl::GetCharScore(uint32_t index, double* p_score)
{
    DAS_UTILS_CHECK_POINTER(p_score);

    if (index >= static_cast<uint32_t>(char_scores_.size()))
    {
        return DAS_E_OUT_OF_RANGE;
    }

    *p_score = char_scores_[index];
    return DAS_S_OK;
}

DAS_CORE_ORTWRAPPER_NS_END

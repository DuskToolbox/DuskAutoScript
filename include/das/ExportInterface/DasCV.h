#ifndef DAS_CV_H
#define DAS_CV_H

#include <das/ExportInterface/IDasImage.h>

typedef struct DasTemplateMatchResult
{
    double  score;
    DasRect match_rect;
} DasTemplateMatchResult;

typedef enum DasTemplateMatchType
{
    /**
     * @brief 归一化差的平方和（Normalized Sum of Squared Difference）
     */
    DAS_TEMPLATE_MATCH_TYPE_SQDIFF_NORMED = 1,
    /**
     * @brief 归一化互相关（Normalized Cross Correlation ）
     */
    DAS_TEMPLATE_MATCH_TYPE_CCORR_NORMED = 3,
    /**
     * @brief 归一化相关系数，即零均值归一化互相关（Zero-mean Normalized Cross
     * Correlation）
     */
    DAS_TEMPLATE_MATCH_TYPE_CCOEFF_NORMED = 5,
    DAS_TEMPLATE_MATCH_TYPE_FORCE_DWORD = 0x7FFFFFFF
} DasTemplateMatchType;

#ifndef SWIG

DAS_C_API DasResult TemplateMatchBest(
    IDasImage*              p_image,
    IDasImage*              p_template,
    DasTemplateMatchType    type,
    DasTemplateMatchResult* p_out_result);

#endif // SWIG

DAS_DEFINE_RET_TYPE(DasRetTemplateMatchResult, DasTemplateMatchResult);

DAS_API DasRetTemplateMatchResult TemplateMatchBest(
    DasSwigImage         image,
    DasSwigImage         template_image,
    DasTemplateMatchType type);

#endif // DAS_CV_H

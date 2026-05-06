#ifndef DAS_CORE_OCVWRAPPER_CVCUDAIMPL_H
#define DAS_CORE_OCVWRAPPER_CVCUDAIMPL_H

#include <das/Core/OcvWrapper/Config.h>

#ifdef DAS_WITH_CUDA

#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasCv.Implements.hpp>

// {4D8E5987-F0D2-4AC9-9913-485F61EE55D0}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OcvWrapper,
    CvCudaImpl,
    0x4d8e5987,
    0xf0d2,
    0x4ac9,
    0x99,
    0x13,
    0x48,
    0x5f,
    0x61,
    0xee,
    0x55,
    0xd0);

DAS_CORE_OCVWRAPPER_NS_BEGIN

/**
 * @brief CUDA backend implementation of IDasCv.
 *
 * Provides template matching, color conversion, and color filtering
 * using GPU-accelerated OpenCV (cv::cuda) operations.
 *
 * Input images are accessed via IImageBackend::GetGpuMat(), which
 * auto-uploads CPU images to GPU (per D-29 lazy materialize).
 * Output images are created via CudaImageImpl::MakeFromGpuMat().
 *
 * CreateMatchConfig and MatchFeatures fall back to CPU because
 * OpenCV CUDA feature detection may not be available in all builds.
 */
class CvCudaImpl final : public ExportInterface::DasCvImplBase<CvCudaImpl>
{
public:
    CvCudaImpl() = default;

    // ---- Template Match ----
    DAS_IMPL TemplateMatchBest(
        ExportInterface::IDasImage*                p_image,
        ExportInterface::IDasImage*                p_template,
        ExportInterface::DasTemplateMatchType      type,
        ExportInterface::IDasTemplateMatchResult** pp_out_result) override;

    DAS_IMPL TemplateMatchAll(
        ExportInterface::IDasImage*                 p_image,
        ExportInterface::IDasImage*                 p_template,
        ExportInterface::DasTemplateMatchType       type,
        double                                      threshold,
        int32_t                                     max_count,
        ExportInterface::IDasTemplateMatchResults** pp_out_results) override;

    // ---- Feature Match ----
    DAS_IMPL CreateMatchConfig(
        ExportInterface::DasDetectorType     detector_type,
        ExportInterface::DasMatcherType      matcher_type,
        ExportInterface::DasMatchParams      params,
        ExportInterface::IDasCvMatchConfig** pp_out_config) override;

    DAS_IMPL MatchFeatures(
        ExportInterface::IDasImage*          p_query,
        ExportInterface::IDasImage*          p_train,
        ExportInterface::IDasCvMatchConfig*  p_config,
        ExportInterface::IDasCvMatchResult** pp_out_result) override;

    // ---- Color Operations ----
    DAS_IMPL ConvertColor(
        ExportInterface::IDasImage*          p_src,
        ExportInterface::DasImagePixelFormat target_format,
        ExportInterface::IDasImage**         pp_out_image) override;

    DAS_IMPL ColorFilter(
        ExportInterface::IDasImage*           p_src,
        const ExportInterface::DasColorRange* p_range,
        ExportInterface::IDasImage**          pp_out_mask) override;
};

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_WITH_CUDA
#endif // DAS_CORE_OCVWRAPPER_CVCUDAIMPL_H

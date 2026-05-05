#ifndef DAS_CORE_OCVWRAPPER_CPUIMAGEIMPL_H
#define DAS_CORE_OCVWRAPPER_CPUIMAGEIMPL_H

#include "Config.h"
#include "IImageBackend.h"

#include <atomic>

DAS_CORE_OCVWRAPPER_NS_BEGIN

/**
 * @brief CPU-favored image implementation.
 *
 * cpu_mat_ is always valid (RAII initialized in constructor).
 * gpu_mat_ is an optional lazy-upload cache.
 *
 * QueryInterface supports IDasBase, IDasImage, and IImageBackend.
 */
class CpuImageImpl final : public IImageBackend
{
    std::atomic<uint32_t>                ref_count_{0};
    cv::Mat                              cpu_mat_;
    ExportInterface::DasImagePixelFormat pixel_format_{
        ExportInterface::DAS_PIXEL_FORMAT_BGR};
#ifdef DAS_WITH_CUDA
    std::optional<cv::cuda::GpuMat> gpu_mat_;
#endif

protected:
    ~CpuImageImpl() = default;

public:
    /// @brief Construct from raw pixel data (takes ownership via clone)
    CpuImageImpl(
        int                                  height,
        int                                  width,
        int                                  type,
        void*                                p_data,
        ExportInterface::IDasMemory*         p_das_data,
        ExportInterface::DasImagePixelFormat format =
            ExportInterface::DAS_PIXEL_FORMAT_BGR);

    /// @brief Construct from existing cv::Mat (takes ownership)
    CpuImageImpl(
        cv::Mat                              mat,
        ExportInterface::DasImagePixelFormat format =
            ExportInterface::DAS_PIXEL_FORMAT_BGR);

    // ---- IUnknown ----
    uint32_t DAS_STD_CALL AddRef() override;
    uint32_t DAS_STD_CALL Release() override;
    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out_object) override;

    // ---- IDasImage ----
    DAS_IMPL GetSize(ExportInterface::DasSize* p_out_size) override;
    DAS_IMPL GetChannelCount(int32_t* p_out_channel_count) override;
    DAS_IMPL Clip(
        const Das::ExportInterface::DasRect* p_rect,
        Das::ExportInterface::IDasImage**    pp_out_image) override;
    DAS_IMPL GetDataSize(uint64_t* p_out_size) override;
    DAS_IMPL GetBinaryBuffer(
        Das::ExportInterface::IDasBinaryBuffer** pp_out_buffer) override;
    DAS_IMPL
    GetPixelFormat(ExportInterface::DasImagePixelFormat* p_out_format) override;

    // ---- IImageBackend ----
    cv::Mat& GetCpuMat() override { return cpu_mat_; }

    bool HasCpuMat() const override { return true; }

    bool HasGpuMat() const override;

    ExportInterface::DasImagePixelFormat GetPixelFormatValue() const override
    {
        return pixel_format_;
    }

    void SetPixelFormat(ExportInterface::DasImagePixelFormat format) override
    {
        pixel_format_ = format;
    }

#ifdef DAS_WITH_CUDA
    cv::cuda::GpuMat& GetGpuMat() override;
#endif

    // ---- Factory ----
    static CpuImageImpl* MakeFromCpuMat(
        cv::Mat                              mat,
        ExportInterface::DasImagePixelFormat format =
            ExportInterface::DAS_PIXEL_FORMAT_BGR);
};

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_CPUIMAGEIMPL_H

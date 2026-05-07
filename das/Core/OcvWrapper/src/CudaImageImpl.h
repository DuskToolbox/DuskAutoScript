#ifndef DAS_CORE_OCVWRAPPER_CUDAIMAGEIMPL_H
#define DAS_CORE_OCVWRAPPER_CUDAIMAGEIMPL_H

#include <das/Core/OcvWrapper/Config.h>
#include <das/Core/OcvWrapper/IImageBackend.h>

#include <das/_autogen/idl/abi/IDasBinaryBuffer.h>

#include <atomic>
#include <optional>

DAS_CORE_OCVWRAPPER_NS_BEGIN

#ifdef DAS_WITH_CUDA

/**
 * @brief GPU-favored image implementation.
 *
 * gpu_mat_ is always valid (RAII initialized in constructor).
 * cpu_mat_ is an optional lazy-download cache.
 *
 * QueryInterface supports IDasBase, IDasImage, IImageBackend, and
 * IDasBinaryBuffer.
 */
class CudaImageImpl final : public IImageBackend,
                            public ExportInterface::IDasBinaryBuffer
{
    std::atomic<uint32_t>                ref_count_{0};
    cv::cuda::GpuMat                     gpu_mat_;
    ExportInterface::DasImagePixelFormat pixel_format_{
        ExportInterface::DAS_PIXEL_FORMAT_BGR};
    std::optional<cv::Mat> cpu_mat_;

protected:
    ~CudaImageImpl() = default;

public:
    /// @brief Construct from existing GpuMat (takes ownership)
    explicit CudaImageImpl(
        cv::cuda::GpuMat                     gpu_mat,
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
        const DAS::ExportInterface::DasRect* p_rect,
        DAS::ExportInterface::IDasImage**    pp_out_image) override;
    DAS_IMPL GetDataSize(uint64_t* p_out_size) override;
    DAS_IMPL GetBinaryBuffer(
        DAS::ExportInterface::IDasBinaryBuffer** pp_out_buffer) override;
    DAS_IMPL
    GetPixelFormat(ExportInterface::DasImagePixelFormat* p_out_format) override;

    // ---- IDasBinaryBuffer ----
    DAS_IMPL GetData(unsigned char** pp_out_data) override;
    DAS_IMPL GetSize(uint64_t* p_out_size) override;

    // ---- IImageBackend ----
    cv::Mat& GetCpuMat() override;
    bool     HasCpuMat() const override { return cpu_mat_.has_value(); }
    bool     HasGpuMat() const override { return true; }

    ExportInterface::DasImagePixelFormat GetPixelFormatValue() const override
    {
        return pixel_format_;
    }

    void SetPixelFormat(ExportInterface::DasImagePixelFormat format) override
    {
        pixel_format_ = format;
    }

    cv::cuda::GpuMat& GetGpuMat() override { return gpu_mat_; }

    // ---- Factory ----
    static CudaImageImpl* MakeFromGpuMat(
        cv::cuda::GpuMat                     gpu_mat,
        ExportInterface::DasImagePixelFormat format =
            ExportInterface::DAS_PIXEL_FORMAT_BGR);
};

#else // !DAS_WITH_CUDA

/// @brief Stub for non-CUDA builds — not constructable.
class CudaImageImpl
{
public:
    CudaImageImpl() = delete;
};

#endif // DAS_WITH_CUDA

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_CUDAIMAGEIMPL_H

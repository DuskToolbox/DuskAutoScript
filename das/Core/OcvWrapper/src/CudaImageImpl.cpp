#include "CudaImageImpl.h"
#include <das/Core/OcvWrapper/Config.h>
#include <das/Core/OcvWrapper/CpuImageImpl.hpp>
#include <das/DasPtr.hpp>

#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasBinaryBuffer.h>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>

DAS_DISABLE_WARNING_END

#include <utility>

DAS_CORE_OCVWRAPPER_NS_BEGIN

#ifdef DAS_WITH_CUDA

// ==================== Constructor ====================

CudaImageImpl::CudaImageImpl(
    cv::cuda::GpuMat                     gpu_mat,
    ExportInterface::DasImagePixelFormat format)
    : gpu_mat_{std::move(gpu_mat)}, pixel_format_{format}
{
}

// ==================== IUnknown ====================

uint32_t DAS_STD_CALL CudaImageImpl::AddRef() { return ++ref_count_; }

uint32_t DAS_STD_CALL CudaImageImpl::Release()
{
    const auto count = --ref_count_;
    if (count == 0)
    {
        delete this;
        return 0;
    }
    return count;
}

DasResult DAS_STD_CALL
CudaImageImpl::QueryInterface(const DasGuid& iid, void** pp_out_object)
{
    if (pp_out_object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    if (iid == DasIidOf<IDasBase>()
        || iid == DasIidOf<ExportInterface::IDasImage>()
        || iid == DasIidOf<Das::Core::OcvWrapper::IImageBackend>())
    {
        *pp_out_object = static_cast<IImageBackend*>(this);
        AddRef();
        return DAS_S_OK;
    }

    if (iid == DasIidOf<ExportInterface::IDasBinaryBuffer>())
    {
        *pp_out_object = static_cast<ExportInterface::IDasBinaryBuffer*>(this);
        AddRef();
        return DAS_S_OK;
    }

    *pp_out_object = nullptr;
    return DAS_E_NO_INTERFACE;
}

// ==================== IDasImage ====================

DasResult CudaImageImpl::GetSize(ExportInterface::DasSize* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size)
    p_out_size->width = gpu_mat_.cols;
    p_out_size->height = gpu_mat_.rows;
    return DAS_S_OK;
}

DasResult CudaImageImpl::GetChannelCount(int32_t* p_out_channel_count)
{
    DAS_UTILS_CHECK_POINTER(p_out_channel_count)
    *p_out_channel_count = gpu_mat_.channels();
    return DAS_S_OK;
}

DasResult CudaImageImpl::Clip(
    const Das::ExportInterface::DasRect* p_rect,
    Das::ExportInterface::IDasImage**    pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_rect)
    DAS_UTILS_CHECK_POINTER(pp_out_image)

    try
    {
        // For GPU-favored images, download to CPU to do the clip
        // (GPU clip operations are more complex and not needed here)
        auto&       cpu_mat = GetCpuMat();
        const auto& rect = *p_rect;
        const auto  clipped_mat = cpu_mat(DAS::Core::OcvWrapper::ToMat(rect));
        auto* const p_result =
            CpuImageImpl<Storage::OwningStorage>::MakeFromCpuMat(
                clipped_mat.clone(),
                pixel_format_);
        p_result->AddRef();
        *pp_out_image = p_result;
        return DAS_S_OK;
    }
    catch (std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult CudaImageImpl::GetDataSize(uint64_t* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size)
    auto&    cpu_mat = GetCpuMat();
    uint64_t result = cpu_mat.total();
    result *= cpu_mat.elemSize1();
    *p_out_size = result;
    return DAS_S_OK;
}

DasResult CudaImageImpl::GetBinaryBuffer(
    Das::ExportInterface::IDasBinaryBuffer** pp_out_buffer)
{
    DAS_UTILS_CHECK_POINTER(pp_out_buffer);
    *pp_out_buffer = static_cast<ExportInterface::IDasBinaryBuffer*>(this);
    AddRef();
    return DAS_S_OK;
}

DasResult CudaImageImpl::GetPixelFormat(
    ExportInterface::DasImagePixelFormat* p_out_format)
{
    DAS_UTILS_CHECK_POINTER(p_out_format)
    *p_out_format = pixel_format_;
    return DAS_S_OK;
}

// ==================== IDasBinaryBuffer ====================

DasResult CudaImageImpl::GetData(unsigned char** pp_out_data)
{
    DAS_UTILS_CHECK_POINTER(pp_out_data)
    *pp_out_data = GetCpuMat().data;
    return DAS_S_OK;
}

DasResult CudaImageImpl::GetSize(uint64_t* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size)
    auto& mat = GetCpuMat();
    *p_out_size = mat.total() * mat.elemSize();
    return DAS_S_OK;
}

// ==================== IImageBackend ====================

cv::Mat& CudaImageImpl::GetCpuMat()
{
    if (!cpu_mat_.has_value())
    {
        cv::Mat tmp;
        gpu_mat_.download(tmp);
        cpu_mat_.emplace(std::move(tmp));
    }
    return cpu_mat_.value();
}

// ==================== Factory ====================

CudaImageImpl* CudaImageImpl::MakeFromGpuMat(
    cv::cuda::GpuMat                     gpu_mat,
    ExportInterface::DasImagePixelFormat format)
{
    auto* p = new CudaImageImpl{std::move(gpu_mat), format};
    p->AddRef();
    return p;
}

#endif // DAS_WITH_CUDA

DAS_CORE_OCVWRAPPER_NS_END

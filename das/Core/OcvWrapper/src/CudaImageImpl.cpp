#include "CudaImageImpl.h"
#include "Config.h"
#include "CpuImageImpl.h"
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasBinaryBuffer.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasBinaryBuffer.Implements.hpp>

#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>

DAS_DISABLE_WARNING_END

#include <cstring>
#include <memory>
#include <utility>

DAS_CORE_OCVWRAPPER_NS_BEGIN

#ifdef DAS_WITH_CUDA

namespace
{
    class DasBinaryBufferImpl final
        : public DAS::ExportInterface::DasBinaryBufferImplBase<
              DasBinaryBufferImpl>
    {
    public:
        explicit DasBinaryBufferImpl(const size_t size_in_bytes)
            : size_{size_in_bytes}
        {
            up_data_ = std::make_unique<unsigned char[]>(size_in_bytes);
        }

        DAS_IMPL GetData(unsigned char** pp_out_data) override
        {
            DAS_UTILS_CHECK_POINTER(pp_out_data);
            *pp_out_data = up_data_.get();
            return DAS_S_OK;
        }

        DAS_IMPL GetSize(uint64_t* p_out_size) override
        {
            DAS_UTILS_CHECK_POINTER(p_out_size);
            *p_out_size = size_;
            return DAS_S_OK;
        }

    private:
        size_t                           size_;
        std::unique_ptr<unsigned char[]> up_data_;
    };
} // unnamed namespace

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

    if (iid == DasIidOf<ExportInterface::IDasBase>()
        || iid == DasIidOf<ExportInterface::IDasImage>()
        || iid == DasIidOf<Das::Core::OcvWrapper::IImageBackend>())
    {
        *pp_out_object = static_cast<IImageBackend*>(this);
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
            CpuImageImpl::MakeFromCpuMat(clipped_mat.clone(), pixel_format_);
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

    try
    {
        auto&      cpu_mat = GetCpuMat();
        uint64_t   data_size{};
        const auto get_size_result = GetDataSize(&data_size);
        if (DAS::IsFailed(get_size_result))
        {
            return get_size_result;
        }

        auto* const    p_buffer = DasBinaryBufferImpl::MakeRaw(data_size);
        unsigned char* p_buffer_data{};
        const auto     get_data_result = p_buffer->GetData(&p_buffer_data);
        if (DAS::IsFailed(get_data_result))
        {
            p_buffer->Release();
            return get_data_result;
        }

        std::memcpy(p_buffer_data, cpu_mat.data, data_size);

        *pp_out_buffer = p_buffer;
        return DAS_S_OK;
    }
    catch (std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult CudaImageImpl::GetPixelFormat(
    ExportInterface::DasImagePixelFormat* p_out_format)
{
    DAS_UTILS_CHECK_POINTER(p_out_format)
    *p_out_format = pixel_format_;
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

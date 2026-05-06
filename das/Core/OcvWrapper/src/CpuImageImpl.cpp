#include "CpuImageImpl.h"
#include "Config.h"
#include "IDasMemory.h"
#include <das/DasApi.h>
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

// ==================== Constructors ====================

CpuImageImpl::CpuImageImpl(
    int   height,
    int   width,
    int   type,
    void* p_data,
    ExportInterface::IDasMemory* /*p_das_data*/,
    ExportInterface::DasImagePixelFormat format)
    : cpu_mat_{height, width, type, p_data}, pixel_format_{format}
{
}

CpuImageImpl::CpuImageImpl(
    cv::Mat                              mat,
    ExportInterface::DasImagePixelFormat format)
    : cpu_mat_{std::move(mat)}, pixel_format_{format}
{
}

// ==================== IUnknown ====================

uint32_t DAS_STD_CALL CpuImageImpl::AddRef() { return ++ref_count_; }

uint32_t DAS_STD_CALL CpuImageImpl::Release()
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
CpuImageImpl::QueryInterface(const DasGuid& iid, void** pp_out_object)
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

DasResult CpuImageImpl::GetSize(ExportInterface::DasSize* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size)
    p_out_size->width = cpu_mat_.cols;
    p_out_size->height = cpu_mat_.rows;
    return DAS_S_OK;
}

DasResult CpuImageImpl::GetChannelCount(int32_t* p_out_channel_count)
{
    DAS_UTILS_CHECK_POINTER(p_out_channel_count)
    *p_out_channel_count = cpu_mat_.channels();
    return DAS_S_OK;
}

DasResult CpuImageImpl::Clip(
    const Das::ExportInterface::DasRect* p_rect,
    Das::ExportInterface::IDasImage**    pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_rect)
    DAS_UTILS_CHECK_POINTER(pp_out_image)

    try
    {
        const auto& rect = *p_rect;
        const auto  clipped_mat = cpu_mat_(DAS::Core::OcvWrapper::ToMat(rect));
        auto* const p_result =
            MakeFromCpuMat(clipped_mat.clone(), pixel_format_);
        p_result->AddRef();
        *pp_out_image = p_result;
        return DAS_S_OK;
    }
    catch (std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult CpuImageImpl::GetDataSize(uint64_t* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size)

    uint64_t result = cpu_mat_.total();
    result *= cpu_mat_.elemSize1();
    *p_out_size = result;

    return DAS_S_OK;
}

DasResult CpuImageImpl::GetBinaryBuffer(
    Das::ExportInterface::IDasBinaryBuffer** pp_out_buffer)
{
    DAS_UTILS_CHECK_POINTER(pp_out_buffer);

    try
    {
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

        std::memcpy(p_buffer_data, cpu_mat_.data, data_size);

        *pp_out_buffer = p_buffer;
        return DAS_S_OK;
    }
    catch (std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult CpuImageImpl::GetPixelFormat(
    ExportInterface::DasImagePixelFormat* p_out_format)
{
    DAS_UTILS_CHECK_POINTER(p_out_format)
    *p_out_format = pixel_format_;
    return DAS_S_OK;
}

// ==================== IImageBackend ====================

bool CpuImageImpl::HasGpuMat() const
{
#ifdef DAS_WITH_CUDA
    return gpu_mat_.has_value();
#else
    return false;
#endif
}

#ifdef DAS_WITH_CUDA
cv::cuda::GpuMat& CpuImageImpl::GetGpuMat()
{
    if (!gpu_mat_.has_value())
    {
        gpu_mat_.emplace();
        gpu_mat_->upload(cpu_mat_);
    }
    return gpu_mat_.value();
}
#endif

// ==================== Factory ====================

CpuImageImpl* CpuImageImpl::MakeFromCpuMat(
    cv::Mat                              mat,
    ExportInterface::DasImagePixelFormat format)
{
    auto* p = new CpuImageImpl{std::move(mat), format};
    p->AddRef();
    return p;
}

DAS_CORE_OCVWRAPPER_NS_END

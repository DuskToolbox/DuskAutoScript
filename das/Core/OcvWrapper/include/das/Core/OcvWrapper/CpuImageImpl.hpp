#ifndef DAS_CORE_OCVWRAPPER_CPUIMAGEIMPL_HPP
#define DAS_CORE_OCVWRAPPER_CPUIMAGEIMPL_HPP

#include <das/Core/OcvWrapper/Config.h>
#include <das/Core/OcvWrapper/IImageBackend.h>
#include <das/Core/Utils/BinaryBuffer.h>

#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasMemory.h>

#include <atomic>
#include <cstring>
#include <optional>

DAS_CORE_OCVWRAPPER_NS_BEGIN

// ==================== Storage Policies ====================

namespace Storage
{

    /// @brief Owning storage: holds a cv::Mat via move semantics.
    class OwningStorage
    {
        cv::Mat mat_;

    public:
        explicit OwningStorage(cv::Mat mat) : mat_(std::move(mat)) {}

        cv::Mat& GetCpuMat() { return mat_; }
    };

    /// @brief Non-owning storage: holds IDasMemory via DasPtr (keeps data
    /// alive)
    ///        and a cv::Mat header pointing into the IDasMemory buffer.
    class IDasMemoryStorage
    {
        DAS::DasPtr<ExportInterface::IDasMemory> memory_;
        cv::Mat                                  mat_;

    public:
        IDasMemoryStorage(
            int                          height,
            int                          width,
            int                          type,
            void*                        p_data,
            ExportInterface::IDasMemory* p_memory)
            : memory_(p_memory), mat_{height, width, type, p_data}
        {
        }

        cv::Mat& GetCpuMat() { return mat_; }
    };

} // namespace Storage

// ==================== CpuImageImpl<Storage> ====================

/**
 * @brief CPU image implementation with configurable storage policy.
 *
 * @tparam Storage Provides cv::Mat& GetCpuMat(). Two instantiations:
 *         - CpuImageImpl<OwningStorage>: owns the cv::Mat data
 *         - CpuImageImpl<IDasMemoryStorage>: non-owning, IDasMemory keeps
 *           buffer alive via DasPtr
 *
 * Both instantiations share the same COM IID (per D-06).
 * QueryInterface supports IDasBase, IDasImage, and IImageBackend.
 */
template <class Storage>
class CpuImageImpl final : public IImageBackend
{
    std::atomic<uint32_t>                ref_count_{0};
    Storage                              storage_;
    ExportInterface::DasImagePixelFormat pixel_format_{
        ExportInterface::DAS_PIXEL_FORMAT_BGR};
#ifdef DAS_WITH_CUDA
    std::optional<cv::cuda::GpuMat> gpu_mat_;
#endif

protected:
    ~CpuImageImpl() = default;

public:
    /// @brief Construct with storage forwarded to Storage constructor
    template <class... Args>
    explicit CpuImageImpl(
        ExportInterface::DasImagePixelFormat format,
        Args&&... args)
        : storage_{std::forward<Args>(args)...}, pixel_format_{format}
    {
    }

    // ---- IUnknown ----

    uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

    uint32_t DAS_STD_CALL Release() override
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
    QueryInterface(const DasGuid& iid, void** pp_out_object) override
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

        *pp_out_object = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    // ---- IDasImage ----

    DAS_IMPL GetSize(ExportInterface::DasSize* p_out_size) override
    {
        DAS_UTILS_CHECK_POINTER(p_out_size)
        auto& mat = storage_.GetCpuMat();
        p_out_size->width = mat.cols;
        p_out_size->height = mat.rows;
        return DAS_S_OK;
    }

    DAS_IMPL GetChannelCount(int32_t* p_out_channel_count) override
    {
        DAS_UTILS_CHECK_POINTER(p_out_channel_count)
        *p_out_channel_count = storage_.GetCpuMat().channels();
        return DAS_S_OK;
    }

    DAS_IMPL Clip(
        const Das::ExportInterface::DasRect* p_rect,
        Das::ExportInterface::IDasImage**    pp_out_image) override
    {
        DAS_UTILS_CHECK_POINTER(p_rect)
        DAS_UTILS_CHECK_POINTER(pp_out_image)

        try
        {
            const auto& rect = *p_rect;
            const auto  clipped_mat =
                storage_.GetCpuMat()(DAS::Core::OcvWrapper::ToMat(rect));
            auto* const p_result =
                CpuImageImpl<DAS::Core::OcvWrapper::Storage::OwningStorage>::
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

    DAS_IMPL GetDataSize(uint64_t* p_out_size) override
    {
        DAS_UTILS_CHECK_POINTER(p_out_size)

        auto&    mat = storage_.GetCpuMat();
        uint64_t result = mat.total();
        result *= mat.elemSize();
        *p_out_size = result;

        return DAS_S_OK;
    }

    DAS_IMPL GetBinaryBuffer(
        Das::ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
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

            auto* const p_buffer =
                DAS::Core::Utils::DasBinaryBufferImpl::MakeRaw(data_size);
            unsigned char* p_buffer_data{};
            const auto     get_data_result = p_buffer->GetData(&p_buffer_data);
            if (DAS::IsFailed(get_data_result))
            {
                p_buffer->Release();
                return get_data_result;
            }

            std::memcpy(p_buffer_data, storage_.GetCpuMat().data, data_size);

            *pp_out_buffer = p_buffer;
            return DAS_S_OK;
        }
        catch (std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }

    DAS_IMPL GetPixelFormat(
        ExportInterface::DasImagePixelFormat* p_out_format) override
    {
        DAS_UTILS_CHECK_POINTER(p_out_format)
        *p_out_format = pixel_format_;
        return DAS_S_OK;
    }

    // ---- IImageBackend ----

    cv::Mat& GetCpuMat() override { return storage_.GetCpuMat(); }

    bool HasCpuMat() const override { return true; }

    bool HasGpuMat() const override
    {
#ifdef DAS_WITH_CUDA
        return gpu_mat_.has_value();
#else
        return false;
#endif
    }

    ExportInterface::DasImagePixelFormat GetPixelFormatValue() const override
    {
        return pixel_format_;
    }

    void SetPixelFormat(ExportInterface::DasImagePixelFormat format) override
    {
        pixel_format_ = format;
    }

#ifdef DAS_WITH_CUDA
    cv::cuda::GpuMat& GetGpuMat() override
    {
        if (!gpu_mat_.has_value())
        {
            gpu_mat_.emplace();
            gpu_mat_->upload(storage_.GetCpuMat());
        }
        return gpu_mat_.value();
    }
#endif

    // ---- Factory ----

    static CpuImageImpl* MakeFromCpuMat(
        cv::Mat                              mat,
        ExportInterface::DasImagePixelFormat format =
            ExportInterface::DAS_PIXEL_FORMAT_BGR)
    {
        auto* p = new CpuImageImpl{format, std::move(mat)};
        p->AddRef();
        return p;
    }
};

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_CPUIMAGEIMPL_HPP

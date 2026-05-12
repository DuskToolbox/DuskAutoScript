#ifndef DAS_CORE_OCVWRAPPER_CPUIMAGEIMPL_HPP
#define DAS_CORE_OCVWRAPPER_CPUIMAGEIMPL_HPP

#include <das/Core/OcvWrapper/Config.h>
#include <das/Core/OcvWrapper/IImageBackend.h>
#include <das/Core/Utils/BinaryBuffer.h>

#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasBinaryBuffer.h>
#include <das/_autogen/idl/abi/IDasMemory.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>

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

    class StorageValidationError final : public std::runtime_error
    {
        DasResult result_;

    public:
        StorageValidationError(DasResult result, const char* p_message)
            : std::runtime_error(p_message), result_(result)
        {
        }

        [[nodiscard]]
        DasResult Result() const noexcept
        {
            return result_;
        }
    };

    /// @brief Buffer-backed storage: holds the exact returned IDasBinaryBuffer
    /// view and a cv::Mat header pointing into that view.
    class IDasBinaryBufferStorage
    {
        DAS::DasPtr<ExportInterface::IDasBinaryBuffer> buffer_;
        cv::Mat                                        mat_;

    public:
        IDasBinaryBufferStorage(
            int                                height,
            int                                width,
            int                                type,
            ExportInterface::IDasBinaryBuffer* p_buffer)
            : buffer_(p_buffer)
        {
            if (p_buffer == nullptr)
            {
                throw StorageValidationError{
                    DAS_E_INVALID_POINTER,
                    "IDasBinaryBufferStorage: p_buffer is null"};
            }

            const auto expected_size =
                ComputeExpectedSize(height, width, CV_ELEM_SIZE(type));

            uint64_t buffer_size{};
            if (const auto result = p_buffer->GetSize(&buffer_size);
                DAS::IsFailed(result))
            {
                throw StorageValidationError{
                    result,
                    "IDasBinaryBufferStorage: GetSize failed"};
            }

            if (buffer_size < expected_size)
            {
                throw StorageValidationError{
                    DAS_E_OUT_OF_RANGE,
                    "IDasBinaryBufferStorage: buffer too small"};
            }

            unsigned char* p_data{};
            if (const auto result = p_buffer->GetData(&p_data);
                DAS::IsFailed(result))
            {
                throw StorageValidationError{
                    result,
                    "IDasBinaryBufferStorage: GetData failed"};
            }

            if (p_data == nullptr)
            {
                throw StorageValidationError{
                    DAS_E_INVALID_POINTER,
                    "IDasBinaryBufferStorage: buffer data is null"};
            }

            mat_ = cv::Mat{height, width, type, p_data};
        }

        cv::Mat& GetCpuMat() { return mat_; }

    private:
        static uint64_t ComputeExpectedSize(
            int    height,
            int    width,
            size_t element_size)
        {
            if (height <= 0 || width <= 0)
            {
                throw StorageValidationError{
                    DAS_E_INVALID_SIZE,
                    "IDasBinaryBufferStorage: invalid dimensions"};
            }

            if (element_size == 0)
            {
                throw StorageValidationError{
                    DAS_E_INVALID_ARGUMENT,
                    "IDasBinaryBufferStorage: invalid element size"};
            }

            const auto height_size = static_cast<uint64_t>(height);
            const auto width_size = static_cast<uint64_t>(width);
            const auto elem_size = static_cast<uint64_t>(element_size);
            const auto max_size = std::numeric_limits<uint64_t>::max();

            if (width_size > max_size / height_size)
            {
                throw StorageValidationError{
                    DAS_E_OUT_OF_RANGE,
                    "IDasBinaryBufferStorage: pixel count overflows"};
            }

            const auto pixel_count = width_size * height_size;
            if (pixel_count > max_size / elem_size)
            {
                throw StorageValidationError{
                    DAS_E_OUT_OF_RANGE,
                    "IDasBinaryBufferStorage: byte count overflows"};
            }

            return pixel_count * elem_size;
        }
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
 *         - CpuImageImpl<IDasBinaryBufferStorage>: non-owning, returned
 *           IDasBinaryBuffer view keeps buffer data alive via DasPtr
 *
 * Both instantiations share the same COM IID (per D-06).
 * QueryInterface supports IDasBase, IDasImage, IImageBackend, and
 * IDasBinaryBuffer.
 */
template <class Storage>
class CpuImageImpl final : public IImageBackend,
                           public ExportInterface::IDasBinaryBuffer
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
            || iid == DasIidOf<DAS::Core::OcvWrapper::IImageBackend>())
        {
            *pp_out_object = static_cast<IImageBackend*>(this);
            AddRef();
            return DAS_S_OK;
        }

        if (iid == DasIidOf<ExportInterface::IDasBinaryBuffer>())
        {
            *pp_out_object =
                static_cast<ExportInterface::IDasBinaryBuffer*>(this);
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
        const DAS::ExportInterface::DasRect* p_rect,
        DAS::ExportInterface::IDasImage**    pp_out_image) override
    {
        DAS_UTILS_CHECK_POINTER(p_rect)
        DAS_UTILS_CHECK_POINTER(pp_out_image)

        try
        {
            const auto& rect = *p_rect;
            if (!DAS::Core::OcvWrapper::IsValidClipRect(
                    rect,
                    storage_.GetCpuMat().cols,
                    storage_.GetCpuMat().rows))
            {
                DAS_CORE_LOG_ERROR(
                    "CpuImageImpl::Clip: invalid rect x={}, y={}, width={}, "
                    "height={}, image_width={}, image_height={}",
                    rect.x,
                    rect.y,
                    rect.width,
                    rect.height,
                    storage_.GetCpuMat().cols,
                    storage_.GetCpuMat().rows);
                return DAS_E_INVALID_SIZE;
            }

            const auto clipped_mat =
                storage_.GetCpuMat()(DAS::Core::OcvWrapper::ToMat(rect));
            auto* const p_result =
                CpuImageImpl<DAS::Core::OcvWrapper::Storage::OwningStorage>::
                    MakeFromCpuMat(clipped_mat.clone(), pixel_format_);
            DAS::DasOutPtr<DAS::ExportInterface::IDasImage> result(
                pp_out_image);
            result.Set(p_result);
            p_result->Release();
            result.Keep();
            return DAS_S_OK;
        }
        catch (std::bad_alloc&)
        {
            DAS_CORE_LOG_ERROR("CpuImageImpl::Clip: out of memory");
            return DAS_E_OUT_OF_MEMORY;
        }
        catch (const std::exception& ex)
        {
            DAS_CORE_LOG_ERROR(
                "CpuImageImpl::Clip: OpenCV exception: {}",
                ex.what());
            return DAS_E_OPENCV_ERROR;
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
        DAS::ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
    {
        DAS_UTILS_CHECK_POINTER(pp_out_buffer);
        *pp_out_buffer = static_cast<ExportInterface::IDasBinaryBuffer*>(this);
        AddRef();
        return DAS_S_OK;
    }

    DAS_IMPL GetPixelFormat(
        ExportInterface::DasImagePixelFormat* p_out_format) override
    {
        DAS_UTILS_CHECK_POINTER(p_out_format)
        *p_out_format = pixel_format_;
        return DAS_S_OK;
    }

    // ---- IDasBinaryBuffer ----

    DAS_IMPL GetData(unsigned char** pp_out_data) override
    {
        DAS_UTILS_CHECK_POINTER(pp_out_data)
        *pp_out_data = storage_.GetCpuMat().data;
        return DAS_S_OK;
    }

    DAS_IMPL GetSize(uint64_t* p_out_size) override
    {
        DAS_UTILS_CHECK_POINTER(p_out_size)
        auto& mat = storage_.GetCpuMat();
        *p_out_size = mat.total() * mat.elemSize();
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

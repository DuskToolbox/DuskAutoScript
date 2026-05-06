#include "CpuImageImpl.h"
#include "IDasMemory.h"
#include <das/Core/OcvWrapper/Config.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasBinaryBuffer.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasBinaryBuffer.Implements.hpp>

#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/ForeignInterfaceHost/PluginResourceIndex.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <das/Utils/StreamUtils.hpp>
#include <das/_autogen/idl/abi/IDasImage.h>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

DAS_DISABLE_WARNING_END

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto ToOcvType(ExportInterface::DasImageFormat format)
    -> DAS::Utils::Expected<int>
{
    switch (format)
    {
    case Das::ExportInterface::DAS_IMAGE_FORMAT_RGB_888:
        return CV_8UC3;
    case Das::ExportInterface::DAS_IMAGE_FORMAT_RGBA_8888:
        [[fallthrough]];
    case Das::ExportInterface::DAS_IMAGE_FORMAT_RGBX_8888:
        return CV_8UC4;
    default:
        return tl::make_unexpected(DAS_E_INVALID_ENUM);
    }
}

DAS_NS_ANONYMOUS_DETAILS_END

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
    result *= cpu_mat_.elemSize();
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

DAS_NS_ANONYMOUS_DETAILS_BEGIN

[[maybe_unused]]
auto ReadFromFile(const std::filesystem::path& full_path) -> cv::Mat
{
    std::vector<char> binary;
    std::ifstream     ifs{};

    DAS::Utils::EnableStreamException(
        ifs,
        std::ios::badbit | std::ios::failbit,
        [&full_path, &binary](std::ifstream& stream)
        {
            stream.open(full_path, std::ios::binary);
            // Stop eating new lines in binary mode!
            stream.unsetf(std::ios::skipws);
            stream.seekg(0, std::ios::end);
            const auto size = stream.tellg();
            stream.seekg(0, std::ios::beg);

            binary.reserve(size);
            std::copy(
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
                std::back_inserter(binary));
        });

    return cv::imdecode(binary, cv::IMREAD_COLOR);
}

DAS_NS_ANONYMOUS_DETAILS_END

DasResult CreateIDasImageFromEncodedData(
    DasImageDesc*                     p_desc,
    Das::ExportInterface::IDasImage** pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_desc)
    DAS_UTILS_CHECK_POINTER(pp_out_image)

    auto& [p_data, data_size, data_format] = *p_desc;

    const auto int_data_size = static_cast<int>(data_size);
    switch (data_format)
    {
    case Das::ExportInterface::DAS_IMAGE_FORMAT_JPG:
        [[fallthrough]];
    case Das::ExportInterface::DAS_IMAGE_FORMAT_PNG:
    {
        if (data_size == 0)
        {
            return DAS_E_INVALID_SIZE;
        }

        auto mat = cv::imdecode({p_data, int_data_size}, cv::IMREAD_UNCHANGED);
        if (mat.empty())
        {
            return DAS_E_OPENCV_ERROR;
        }
        cv::Mat rgb_mat{};
        cv::cvtColor(mat, rgb_mat, cv::COLOR_BGR2RGB);

        auto* p_result = DAS::Core::OcvWrapper::CpuImageImpl::MakeFromCpuMat(
            std::move(rgb_mat),
            Das::ExportInterface::DAS_PIXEL_FORMAT_RGB);
        *pp_out_image = p_result;
        return DAS_S_OK;
    }
    default:
        return DAS_E_INVALID_ENUM;
    }
}

DasResult CreateIDasImageFromDecodedData(
    const DasImageDesc*                  p_desc,
    const DAS::ExportInterface::DasSize* p_size,
    DAS::ExportInterface::IDasImage**    pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_desc)
    DAS_UTILS_CHECK_POINTER(p_size)
    DAS_UTILS_CHECK_POINTER(pp_out_image)

    auto&      desc = *p_desc;
    auto&      size = *p_size;
    const auto expected_type =
        DAS::Core::OcvWrapper::Details::ToOcvType(desc.data_format);

    if (!expected_type)
    {
        return expected_type.error();
    }

    cv::Mat input_image{
        size.height,
        size.width,
        expected_type.value(),
        desc.p_data};
    auto owned_image = input_image.clone();

    auto* p_result = DAS::Core::OcvWrapper::CpuImageImpl::MakeFromCpuMat(
        std::move(owned_image),
        Das::ExportInterface::DAS_PIXEL_FORMAT_BGR);
    *pp_out_image = p_result;

    return DAS_S_OK;
}

DasResult CreateIDasImageFromRgb888(
    DAS::ExportInterface::IDasMemory*    p_alias_memory,
    const DAS::ExportInterface::DasSize* p_size,
    DAS::ExportInterface::IDasImage**    pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_alias_memory)
    DAS_UTILS_CHECK_POINTER(p_size)
    DAS_UTILS_CHECK_POINTER(pp_out_image)

    const auto&    size = *p_size;
    uint64_t       data_size{};
    unsigned char* p_data{};

    if (const auto get_size_result = p_alias_memory->GetSize(&data_size);
        DAS::IsFailed(get_size_result)) [[unlikely]]
    {
        return get_size_result;
    }

    DAS::ExportInterface::IDasBinaryBuffer* p_buffer = nullptr;
    if (const auto get_buffer_result =
            p_alias_memory->GetBinaryBuffer(&p_buffer);
        DAS::IsFailed(get_buffer_result)) [[unlikely]]
    {
        return get_buffer_result;
    }

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> buffer_guard(p_buffer);

    if (const auto get_data_result = p_buffer->GetData(&p_data);
        DAS::IsFailed(get_data_result)) [[unlikely]]
    {
        return get_data_result;
    }

    const size_t required_size =
        static_cast<size_t>(std::abs(size.height * size.width * 4));
    if (required_size > data_size)
    {
        return DAS_E_OUT_OF_RANGE;
    }

    // Clone the data to ensure ownership — the cv::Mat constructor with
    // external data creates a non-owning header.
    cv::Mat owned{size.height, size.width, CV_8UC4, p_data};
    auto*   p_result = DAS::Core::OcvWrapper::CpuImageImpl::MakeFromCpuMat(
        owned.clone(),
        Das::ExportInterface::DAS_PIXEL_FORMAT_RGBA);
    *pp_out_image = p_result;

    return DAS_S_OK;
}

DasResult DasPluginLoadImageFromResource(
    IDasTypeInfo*                     p_type_info,
    IDasReadOnlyString*               p_relative_path,
    DAS::ExportInterface::IDasImage** pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_type_info)
    DAS_UTILS_CHECK_POINTER(p_relative_path)
    DAS_UTILS_CHECK_POINTER(pp_out_image)

    DAS::DasOutPtr<DAS::ExportInterface::IDasImage> result(pp_out_image);

    DasGuid guid{};
    {
        const auto guid_result = p_type_info->GetGuid(&guid);
        if (DAS::IsFailed(guid_result))
        {
            DAS_CORE_LOG_ERROR("GetGuid() failed, result = {}", guid_result);
            return guid_result;
        }
    }

    const DAS::Core::ForeignInterfaceHost::PluginResourceEntry* p_entry =
        nullptr;
    {
        auto& index =
            DAS::Core::ForeignInterfaceHost::PluginResourceIndex::GetInstance();
        const auto resolve_result =
            index.ResolvePluginResourceEntryByGuid(guid, &p_entry);
        if (DAS::IsFailed(resolve_result))
        {
            DAS_CORE_LOG_ERROR(
                "GUID resolve failed, result = {}",
                resolve_result);
            return resolve_result;
        }
    }

    const char* p_u8_path = nullptr;
    {
        const auto utf8_result = p_relative_path->GetUtf8(&p_u8_path);
        if (DAS::IsFailed(utf8_result))
        {
            DAS_CORE_LOG_ERROR("GetUtf8() failed, result = {}", utf8_result);
            return utf8_result;
        }
    }

    std::filesystem::path full_path;
    {
        auto& index =
            DAS::Core::ForeignInterfaceHost::PluginResourceIndex::GetInstance();
        const auto path_result = index.ResolveResourceFullPath(
            p_entry->resource_root,
            p_u8_path,
            full_path);
        if (DAS::IsFailed(path_result))
        {
            DAS_CORE_LOG_ERROR("path resolve failed, result = {}", path_result);
            return path_result;
        }
    }

    if (!std::filesystem::exists(full_path))
    {
        DAS_CORE_LOG_ERROR("file not found: {}", full_path.string());
        return DAS_E_FILE_NOT_FOUND;
    }

    try
    {
        const auto mat = Details::ReadFromFile(full_path);
        if (mat.empty())
        {
            DAS_CORE_LOG_ERROR(
                "decoded image is empty for file: {}",
                full_path.string());
            return DAS_E_OPENCV_ERROR;
        }

        cv::Mat rgb_mat{};
        cv::cvtColor(mat, rgb_mat, cv::COLOR_BGR2RGB);

        auto* p_result = DAS::Core::OcvWrapper::CpuImageImpl::MakeFromCpuMat(
            std::move(rgb_mat),
            Das::ExportInterface::DAS_PIXEL_FORMAT_RGB);
        result.Set(p_result);
        result.Keep();
        return DAS_S_OK;
    }
    catch (const std::ios_base::failure& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_ERROR("file read failed, result = {}", DAS_E_INVALID_FILE);
        return DAS_E_INVALID_FILE;
    }
    catch (const cv::Exception& ex)
    {
        DAS_CORE_LOG_ERROR("OpenCV decode failed: {}", ex.err);
        DAS_CORE_LOG_ERROR(
            "NOTE:\nfile = {}\nline = {}\nfunction = {}",
            ex.file,
            ex.line,
            ex.func);
        return DAS_E_OPENCV_ERROR;
    }
}

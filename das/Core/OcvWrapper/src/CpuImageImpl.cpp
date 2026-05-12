#include <das/Core/OcvWrapper/Config.h>
#include <das/Core/OcvWrapper/CpuImageImpl.hpp>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasMemory.h>

#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/ForeignInterfaceHost/PluginResourceIndex.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <das/Utils/StreamUtils.hpp>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

DAS_DISABLE_WARNING_END

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>

DAS_CORE_OCVWRAPPER_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto ToOcvType(ExportInterface::DasImageFormat format)
    -> DAS::Utils::Expected<int>
{
    switch (format)
    {
    case DAS::ExportInterface::DAS_IMAGE_FORMAT_RGB_888:
        return CV_8UC3;
    case DAS::ExportInterface::DAS_IMAGE_FORMAT_RGBA_8888:
        [[fallthrough]];
    case DAS::ExportInterface::DAS_IMAGE_FORMAT_RGBX_8888:
        return CV_8UC4;
    default:
        return tl::make_unexpected(DAS_E_INVALID_ENUM);
    }
}

auto ToDecodedPixelFormat(ExportInterface::DasImageFormat format)
    -> DAS::Utils::Expected<ExportInterface::DasImagePixelFormat>
{
    switch (format)
    {
    case ExportInterface::DAS_IMAGE_FORMAT_RGB_888:
        return ExportInterface::DAS_PIXEL_FORMAT_RGB;
    case ExportInterface::DAS_IMAGE_FORMAT_RGBA_8888:
        [[fallthrough]];
    case ExportInterface::DAS_IMAGE_FORMAT_RGBX_8888:
        return ExportInterface::DAS_PIXEL_FORMAT_RGBA;
    default:
        return tl::make_unexpected(DAS_E_INVALID_ENUM);
    }
}

auto ValidateRawImageInput(
    const DasImageDesc&             desc,
    const ExportInterface::DasSize& size,
    size_t                          element_size,
    std::string_view function_name) -> DAS::Utils::Expected<uint64_t>
{
    if (desc.p_data == nullptr)
    {
        DAS_CORE_LOG_ERROR("{}: p_data is null", function_name);
        return tl::make_unexpected(DAS_E_INVALID_POINTER);
    }

    if (size.width <= 0 || size.height <= 0)
    {
        DAS_CORE_LOG_ERROR(
            "{}: invalid image dimensions width={}, height={}",
            function_name,
            size.width,
            size.height);
        return tl::make_unexpected(DAS_E_INVALID_SIZE);
    }

    const auto width = static_cast<uint64_t>(size.width);
    const auto height = static_cast<uint64_t>(size.height);
    const auto bytes_per_pixel = static_cast<uint64_t>(element_size);
    const auto max_size = std::numeric_limits<uint64_t>::max();

    if (width > max_size / height)
    {
        DAS_CORE_LOG_ERROR(
            "{}: image pixel count overflows, width={}, height={}",
            function_name,
            size.width,
            size.height);
        return tl::make_unexpected(DAS_E_OUT_OF_RANGE);
    }

    const auto pixel_count = width * height;
    if (bytes_per_pixel != 0 && pixel_count > max_size / bytes_per_pixel)
    {
        DAS_CORE_LOG_ERROR(
            "{}: image byte count overflows, pixels={}, bytes_per_pixel={}",
            function_name,
            pixel_count,
            bytes_per_pixel);
        return tl::make_unexpected(DAS_E_OUT_OF_RANGE);
    }

    const auto expected_size = pixel_count * bytes_per_pixel;
    if (static_cast<uint64_t>(desc.data_size) < expected_size)
    {
        DAS_CORE_LOG_ERROR(
            "{}: buffer too small, data_size={}, expected_size={}",
            function_name,
            desc.data_size,
            expected_size);
        return tl::make_unexpected(DAS_E_OUT_OF_RANGE);
    }

    return expected_size;
}

DAS_NS_ANONYMOUS_DETAILS_END

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
    DAS::ExportInterface::IDasImage** pp_out_image)
{
    if (p_desc == nullptr)
    {
        DAS_CORE_LOG_ERROR("CreateIDasImageFromEncodedData: p_desc is null");
        return DAS_E_INVALID_POINTER;
    }
    if (pp_out_image == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "CreateIDasImageFromEncodedData: pp_out_image is null");
        return DAS_E_INVALID_POINTER;
    }

    auto& [p_data, data_size, data_format] = *p_desc;

    switch (data_format)
    {
    case DAS::ExportInterface::DAS_IMAGE_FORMAT_JPG:
        [[fallthrough]];
    case DAS::ExportInterface::DAS_IMAGE_FORMAT_PNG:
    {
        if (data_size == 0)
        {
            DAS_CORE_LOG_ERROR(
                "CreateIDasImageFromEncodedData: data_size is zero");
            return DAS_E_INVALID_SIZE;
        }

        if (p_data == nullptr)
        {
            DAS_CORE_LOG_ERROR(
                "CreateIDasImageFromEncodedData: p_data is null");
            return DAS_E_INVALID_POINTER;
        }

        if (data_size > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            DAS_CORE_LOG_ERROR(
                "CreateIDasImageFromEncodedData: data_size too large, size={}",
                data_size);
            return DAS_E_INVALID_SIZE;
        }

        try
        {
            const auto int_data_size = static_cast<int>(data_size);
            cv::Mat    encoded_data{1, int_data_size, CV_8UC1, p_data};
            auto       mat = cv::imdecode(encoded_data, cv::IMREAD_UNCHANGED);
            if (mat.empty())
            {
                DAS_CORE_LOG_ERROR(
                    "CreateIDasImageFromEncodedData: OpenCV decode failed");
                return DAS_E_OPENCV_ERROR;
            }

            cv::Mat normalized_mat{};
            auto pixel_format = DAS::ExportInterface::DAS_PIXEL_FORMAT_UNKNOWN;
            switch (mat.channels())
            {
            case 1:
                normalized_mat = mat.clone();
                pixel_format = DAS::ExportInterface::DAS_PIXEL_FORMAT_GRAY;
                break;
            case 3:
                cv::cvtColor(mat, normalized_mat, cv::COLOR_BGR2RGB);
                pixel_format = DAS::ExportInterface::DAS_PIXEL_FORMAT_RGB;
                break;
            case 4:
                cv::cvtColor(mat, normalized_mat, cv::COLOR_BGRA2RGBA);
                pixel_format = DAS::ExportInterface::DAS_PIXEL_FORMAT_RGBA;
                break;
            default:
                DAS_CORE_LOG_ERROR(
                    "CreateIDasImageFromEncodedData: unsupported channel "
                    "count={}",
                    mat.channels());
                return DAS_E_OPENCV_ERROR;
            }

            DAS::DasOutPtr<DAS::ExportInterface::IDasImage> result(
                pp_out_image);
            auto* p_result = DAS::Core::OcvWrapper::CpuImageImpl<
                DAS::Core::OcvWrapper::Storage::OwningStorage>::
                MakeFromCpuMat(std::move(normalized_mat), pixel_format);
            result.Set(p_result);
            p_result->Release();
            result.Keep();
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            DAS_CORE_LOG_ERROR("CreateIDasImageFromEncodedData: out of memory");
            return DAS_E_OUT_OF_MEMORY;
        }
        catch (const cv::Exception& ex)
        {
            DAS_CORE_LOG_ERROR(
                "CreateIDasImageFromEncodedData: OpenCV exception: {}",
                ex.what());
            return DAS_E_OPENCV_ERROR;
        }
    }
    default:
        DAS_CORE_LOG_ERROR(
            "CreateIDasImageFromEncodedData: invalid data_format={}",
            static_cast<int>(data_format));
        return DAS_E_INVALID_ENUM;
    }
}

DasResult CreateIDasImageFromDecodedData(
    const DasImageDesc*                  p_desc,
    const DAS::ExportInterface::DasSize* p_size,
    DAS::ExportInterface::IDasImage**    pp_out_image)
{
    if (p_desc == nullptr)
    {
        DAS_CORE_LOG_ERROR("CreateIDasImageFromDecodedData: p_desc is null");
        return DAS_E_INVALID_POINTER;
    }
    if (p_size == nullptr)
    {
        DAS_CORE_LOG_ERROR("CreateIDasImageFromDecodedData: p_size is null");
        return DAS_E_INVALID_POINTER;
    }
    if (pp_out_image == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "CreateIDasImageFromDecodedData: pp_out_image is null");
        return DAS_E_INVALID_POINTER;
    }

    auto&      desc = *p_desc;
    auto&      size = *p_size;
    const auto expected_type =
        DAS::Core::OcvWrapper::Details::ToOcvType(desc.data_format);

    if (!expected_type)
    {
        DAS_CORE_LOG_ERROR(
            "CreateIDasImageFromDecodedData: invalid data_format={}",
            static_cast<int>(desc.data_format));
        return expected_type.error();
    }

    const auto expected_format =
        DAS::Core::OcvWrapper::Details::ToDecodedPixelFormat(desc.data_format);
    if (!expected_format)
    {
        DAS_CORE_LOG_ERROR(
            "CreateIDasImageFromDecodedData: no pixel format for data_format={}",
            static_cast<int>(desc.data_format));
        return expected_format.error();
    }

    const auto expected_size =
        DAS::Core::OcvWrapper::Details::ValidateRawImageInput(
            desc,
            size,
            CV_ELEM_SIZE(expected_type.value()),
            "CreateIDasImageFromDecodedData");
    if (!expected_size)
    {
        return expected_size.error();
    }

    try
    {
        cv::Mat input_image{
            size.height,
            size.width,
            expected_type.value(),
            desc.p_data};
        auto owned_image = input_image.clone();

        DAS::DasOutPtr<DAS::ExportInterface::IDasImage> result(pp_out_image);
        auto* p_result = DAS::Core::OcvWrapper::CpuImageImpl<
            DAS::Core::OcvWrapper::Storage::OwningStorage>::
            MakeFromCpuMat(std::move(owned_image), expected_format.value());
        result.Set(p_result);
        p_result->Release();
        result.Keep();
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("CreateIDasImageFromDecodedData: out of memory");
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (const cv::Exception& ex)
    {
        DAS_CORE_LOG_ERROR(
            "CreateIDasImageFromDecodedData: OpenCV exception: {}",
            ex.what());
        return DAS_E_OPENCV_ERROR;
    }

    return DAS_S_OK;
}

DasResult CreateIDasImageFromRgb888(
    DAS::ExportInterface::IDasMemory*    p_alias_memory,
    const DAS::ExportInterface::DasSize* p_size,
    DAS::ExportInterface::IDasImage**    pp_out_image)
{
    if (p_alias_memory == nullptr)
    {
        DAS_CORE_LOG_ERROR("CreateIDasImageFromRgb888: p_alias_memory is null");
        return DAS_E_INVALID_POINTER;
    }
    if (p_size == nullptr)
    {
        DAS_CORE_LOG_ERROR("CreateIDasImageFromRgb888: p_size is null");
        return DAS_E_INVALID_POINTER;
    }
    if (pp_out_image == nullptr)
    {
        DAS_CORE_LOG_ERROR("CreateIDasImageFromRgb888: pp_out_image is null");
        return DAS_E_INVALID_POINTER;
    }

    const auto&    size = *p_size;
    uint64_t       data_size{};
    unsigned char* p_data{};

    DAS::ExportInterface::IDasBinaryBuffer* p_buffer = nullptr;
    if (const auto get_buffer_result =
            p_alias_memory->GetBinaryBuffer(0, &p_buffer);
        DAS::IsFailed(get_buffer_result)) [[unlikely]]
    {
        DAS_CORE_LOG_ERROR(
            "CreateIDasImageFromRgb888: GetBinaryBuffer failed, result={}",
            get_buffer_result);
        return get_buffer_result;
    }

    auto buffer_guard =
        DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer>::Attach(p_buffer);

    if (const auto get_size_result = p_buffer->GetSize(&data_size);
        DAS::IsFailed(get_size_result)) [[unlikely]]
    {
        DAS_CORE_LOG_ERROR(
            "CreateIDasImageFromRgb888: buffer GetSize failed, result={}",
            get_size_result);
        return get_size_result;
    }

    if (const auto get_data_result = p_buffer->GetData(&p_data);
        DAS::IsFailed(get_data_result)) [[unlikely]]
    {
        DAS_CORE_LOG_ERROR(
            "CreateIDasImageFromRgb888: GetData failed, result={}",
            get_data_result);
        return get_data_result;
    }

    DasImageDesc desc{
        .p_data = reinterpret_cast<char*>(p_data),
        .data_size = static_cast<size_t>(data_size),
        .data_format = DAS::ExportInterface::DAS_IMAGE_FORMAT_RGBA_8888};
    const auto expected_size =
        DAS::Core::OcvWrapper::Details::ValidateRawImageInput(
            desc,
            size,
            4,
            "CreateIDasImageFromRgb888");
    if (!expected_size)
    {
        return expected_size.error();
    }

    try
    {
        // Clone the data to ensure ownership — the cv::Mat constructor with
        // external data creates a non-owning header.
        cv::Mat owned{size.height, size.width, CV_8UC4, p_data};
        DAS::DasOutPtr<DAS::ExportInterface::IDasImage> result(pp_out_image);
        auto* p_result = DAS::Core::OcvWrapper::CpuImageImpl<
            DAS::Core::OcvWrapper::Storage::OwningStorage>::
            MakeFromCpuMat(
                owned.clone(),
                DAS::ExportInterface::DAS_PIXEL_FORMAT_RGBA);
        result.Set(p_result);
        p_result->Release();
        result.Keep();
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("CreateIDasImageFromRgb888: out of memory");
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (const cv::Exception& ex)
    {
        DAS_CORE_LOG_ERROR(
            "CreateIDasImageFromRgb888: OpenCV exception: {}",
            ex.what());
        return DAS_E_OPENCV_ERROR;
    }

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

        auto* p_result = DAS::Core::OcvWrapper::CpuImageImpl<
            DAS::Core::OcvWrapper::Storage::OwningStorage>::
            MakeFromCpuMat(
                std::move(rgb_mat),
                DAS::ExportInterface::DAS_PIXEL_FORMAT_RGB);
        result.Set(p_result);
        p_result->Release();
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

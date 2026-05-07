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

DAS_CORE_OCVWRAPPER_NS_BEGIN

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

        auto* p_result = DAS::Core::OcvWrapper::CpuImageImpl<
            DAS::Core::OcvWrapper::Storage::OwningStorage>::
            MakeFromCpuMat(
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

    auto* p_result = DAS::Core::OcvWrapper::CpuImageImpl<
        DAS::Core::OcvWrapper::Storage::OwningStorage>::
        MakeFromCpuMat(
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
    auto*   p_result = DAS::Core::OcvWrapper::CpuImageImpl<
        DAS::Core::OcvWrapper::Storage::OwningStorage>::
        MakeFromCpuMat(
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

        auto* p_result = DAS::Core::OcvWrapper::CpuImageImpl<
            DAS::Core::OcvWrapper::Storage::OwningStorage>::
            MakeFromCpuMat(
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

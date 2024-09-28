#include "IAsrImageImpl.h"
#include "Config.h"
#include <AutoStarRail/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/PluginManager.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/ExportInterface/IAsrImage.h>
#include <AutoStarRail/Utils/Expected.h>
#include <AutoStarRail/Utils/QueryInterface.hpp>
#include <AutoStarRail/Utils/StreamUtils.hpp>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

ASR_DISABLE_WARNING_BEGIN

ASR_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

ASR_DISABLE_WARNING_END

AsrSwigImage::AsrSwigImage() = default;

AsrSwigImage::AsrSwigImage(ASR::AsrPtr<IAsrImage> p_image)
    : p_image_{std::move(p_image)}
{
}

IAsrImage* AsrSwigImage::Get() const { return p_image_.Get(); }

ASR_CORE_OCVWRAPPER_NS_BEGIN

ASR_NS_ANONYMOUS_DETAILS_BEGIN

auto ToOcvType(AsrImageFormat format) -> ASR::Utils::Expected<int>
{
    switch (format)
    {
    case ASR_IMAGE_FORMAT_RGB_888:
        return CV_8UC3;
    case ASR_IMAGE_FORMAT_RGBA_8888:
        [[fallthrough]];
    case ASR_IMAGE_FORMAT_RGBX_8888:
        return CV_8UC4;
    default:
        return tl::make_unexpected(ASR_E_INVALID_ENUM);
    }
}

ASR_NS_ANONYMOUS_DETAILS_END

IAsrImageImpl::IAsrImageImpl(
    int         height,
    int         width,
    int         type,
    void*       p_data,
    IAsrMemory* p_asr_data)
    : p_memory_{p_asr_data}, mat_{height, width, type, p_data}
{
}

IAsrImageImpl::IAsrImageImpl(cv::Mat mat) : p_memory_{}, mat_{std::move(mat)} {}

AsrResult IAsrImageImpl::QueryInterface(
    const AsrGuid& iid,
    void**         pp_out_object)
{
    return ASR::Utils::QueryInterface<IAsrImage>(this, iid, pp_out_object);
}

AsrResult IAsrImageImpl::GetSize(AsrSize* p_out_size)
{
    ASR_UTILS_CHECK_POINTER(p_out_size)

    p_out_size->width = mat_.cols;
    p_out_size->height = mat_.rows;

    return ASR_S_OK;
}

AsrResult IAsrImageImpl::GetChannelCount(int* p_out_channel_count)
{
    ASR_UTILS_CHECK_POINTER(p_out_channel_count)

    *p_out_channel_count = mat_.channels();

    return ASR_S_OK;
}

AsrResult IAsrImageImpl::Clip(const AsrRect* p_rect, IAsrImage** pp_out_image)
{
    ASR_UTILS_CHECK_POINTER(p_rect)
    ASR_UTILS_CHECK_POINTER(pp_out_image)

    try
    {
        const auto& rect = *p_rect;
        const auto  clipped_mat = mat_(ASR::Core::OcvWrapper::ToMat(rect));
        auto        p_result = new IAsrImageImpl{clipped_mat};
        p_result->p_memory_ = p_memory_;
        p_result->AddRef();
        *pp_out_image = p_result;
        return ASR_S_OK;
    }
    catch (std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult IAsrImageImpl::GetDataSize(size_t* p_out_size)
{
    ASR_UTILS_CHECK_POINTER(p_out_size)

    size_t result = mat_.total();
    result *= mat_.elemSize1();
    *p_out_size = result;

    return ASR_S_OK;
}

AsrResult IAsrImageImpl::CopyTo(unsigned char* p_out_memory)
{
    ASR_UTILS_CHECK_POINTER(p_out_memory)

    try
    {
        size_t data_size;
        GetDataSize(&data_size);
        const auto int_data_size = static_cast<int>(data_size);
        mat_.copyTo({p_out_memory, int_data_size});
    }
    catch (cv::Exception& ex)
    {
        ASR_CORE_LOG_ERROR(ex.err);
        ASR_CORE_LOG_ERROR(
            "NOTE:\nfile = {}\nline = {}\nfunction = {}",
            ex.file,
            ex.line,
            ex.func);
    }

    return ASR_S_OK;
}

auto IAsrImageImpl::GetImpl() -> cv::Mat { return mat_; }

ASR_CORE_OCVWRAPPER_NS_END

AsrResult CreateIAsrImageFromEncodedData(
    AsrImageDesc* p_desc,
    IAsrImage**   pp_out_image)
{
    ASR_UTILS_CHECK_POINTER(p_desc)
    ASR_UTILS_CHECK_POINTER(pp_out_image)

    auto& [p_data, data_size, data_format] = *p_desc;

    const auto int_data_size = static_cast<int>(data_size);
    switch (data_format)
    {
    case ASR_IMAGE_FORMAT_JPG:
        [[fallthrough]];
    case ASR_IMAGE_FORMAT_PNG:
    {
        if (data_size == 0)
        {
            return ASR_E_INVALID_SIZE;
        }

        auto mat = cv::imdecode({p_data, int_data_size}, cv::IMREAD_UNCHANGED);
        cv::Mat rgb_mat{};
        cv::cvtColor(mat, rgb_mat, cv::COLOR_BGR2RGB);

        auto p_result = new ASR::Core::OcvWrapper::IAsrImageImpl{rgb_mat};
        p_result->AddRef();
        *pp_out_image = p_result;
        return ASR_S_OK;
    }
    default:
        return ASR_E_INVALID_ENUM;
    }
}

AsrResult CreateIAsrImageFromDecodedData(
    const AsrImageDesc* p_desc,
    const AsrSize*      p_size,
    IAsrImage**         pp_out_image)
{
    ASR_UTILS_CHECK_POINTER(p_desc)
    ASR_UTILS_CHECK_POINTER(p_size)
    ASR_UTILS_CHECK_POINTER(pp_out_image)

    auto&      desc = *p_desc;
    auto&      size = *p_size;
    const auto expected_type =
        ASR::Core::OcvWrapper::Details::ToOcvType(desc.data_format);

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

    auto p_result = new ASR::Core::OcvWrapper::IAsrImageImpl{owned_image};
    p_result->AddRef();
    *pp_out_image = p_result;

    return ASR_S_OK;
}

AsrResult CreateIAsrImageFromRgb888(
    IAsrMemory*    p_alias_memory,
    const AsrSize* p_size,
    IAsrImage**    pp_out_image)
{
    ASR_UTILS_CHECK_POINTER(p_alias_memory)
    ASR_UTILS_CHECK_POINTER(p_size)
    ASR_UTILS_CHECK_POINTER(pp_out_image)

    const auto&    size = *p_size;
    size_t         data_size;
    unsigned char* p_data;

    if (const auto get_size_result = p_alias_memory->GetSize(&data_size);
        ASR::IsFailed(get_size_result)) [[unlikely]]
    {
        return get_size_result;
    }

    if (const auto get_pointer_result = p_alias_memory->GetData(&p_data);
        ASR::IsFailed(get_pointer_result)) [[unlikely]]
    {
        return get_pointer_result;
    }

    const size_t required_size = std::abs(size.height * size.width * 4);
    if (required_size > data_size)
    {
        return ASR_E_OUT_OF_RANGE;
    }

    auto p_result = new ASR::Core::OcvWrapper::IAsrImageImpl{
        size.height,
        size.width,
        CV_8UC4,
        p_data,
        p_alias_memory};
    p_result->AddRef();
    *pp_out_image = p_result;

    return ASR_S_OK;
}

ASR_NS_ANONYMOUS_DETAILS_BEGIN

auto ReadFromFile(const std::filesystem::path& full_path) -> cv::Mat
{
    std::vector<char> binary;
    std::ifstream     ifs{};

    ASR::Utils::EnableStreamException(
        ifs,
        std::ios::badbit | std::ios::failbit,
        [&full_path, &binary](auto& stream)
        {
            stream.open(full_path, std::ios::binary);
            // Stop eating new lines in binary mode!
            stream.unsetf(std::ios::skipws);
            stream.seekg(0, std::ios::end);
            const auto size = stream.tellg();
            stream.seekg(0, std::ios::beg);

            binary.reserve(size);
            std::copy(
                std::istream_iterator<char>{stream},
                std::istream_iterator<char>{},
                binary.begin());
        });

    return cv::imdecode(binary, cv::IMREAD_COLOR);
}

ASR_NS_ANONYMOUS_DETAILS_END

AsrResult AsrPluginLoadImageFromResource(
    IAsrTypeInfo*       p_type_info,
    IAsrReadOnlyString* p_relative_path,
    IAsrImage**         pp_out_image)
{
    ASR_UTILS_CHECK_POINTER(p_type_info)
    ASR_UTILS_CHECK_POINTER(p_relative_path)
    ASR_UTILS_CHECK_POINTER(pp_out_image)

    const auto expected_storage =
        ASR::Core::ForeignInterfaceHost::g_plugin_manager
            .GetInterfaceStaticStorage(p_type_info);
    if (!expected_storage)
    {
        const auto error_code = expected_storage.error();
        ASR_CORE_LOG_ERROR(
            "Get interface static storage failed. Error code = {}.",
            error_code);
        return error_code;
    }

    const char* p_u8_relative_path{};
    p_relative_path->GetUtf8(&p_u8_relative_path);

    const auto full_path =
        expected_storage.value().get().path / p_u8_relative_path;

    try
    {
        const auto mat = Details::ReadFromFile(full_path);
        auto*      p_result = new Asr::Core::OcvWrapper::IAsrImageImpl{mat};
        *pp_out_image = p_result;
        p_result->AddRef();
        return ASR_S_OK;
    }
    catch (const std::ios_base::failure& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        ASR_CORE_LOG_ERROR(
            "Error happened when reading resource file. Error code = " ASR_STR(
                ASR_E_INVALID_FILE) ".");
        return ASR_E_INVALID_FILE;
    }
    catch (cv::Exception& ex)
    {
        ASR_CORE_LOG_ERROR(ex.err);
        ASR_CORE_LOG_ERROR(
            "NOTE:\nfile = {}\nline = {}\nfunction = {}",
            ex.file,
            ex.line,
            ex.func);
        return ASR_E_OPENCV_ERROR;
    }
}

AsrRetImage AsrPluginLoadImageFromResource(
    IAsrSwigTypeInfo* p_type_info,
    AsrReadOnlyString relative_path)
{
    AsrRetImage            result{};
    ASR::AsrPtr<IAsrImage> p_image;

    Asr::Core::ForeignInterfaceHost::SwigToCpp<IAsrSwigTypeInfo> cpp_type_info{
        p_type_info};

    result.error_code = AsrPluginLoadImageFromResource(
        &cpp_type_info,
        relative_path.Get(),
        p_image.Put());

    if (ASR::IsOk(result.error_code))
    {
        result.value = p_image;
    }

    return result;
}

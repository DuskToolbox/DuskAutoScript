#include "IDasImageImpl.h"
#include "Config.h"
#include <das/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/ExportInterface/IDasImage.h>
#include <das/Utils/Expected.h>
#include <das/Utils/QueryInterface.hpp>
#include <das/Utils/StreamUtils.hpp>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

DAS_DISABLE_WARNING_END

DasSwigImage::DasSwigImage() = default;

DasSwigImage::DasSwigImage(DAS::DasPtr<IDasImage> p_image)
    : p_image_{std::move(p_image)}
{
}

IDasImage* DasSwigImage::Get() const { return p_image_.Get(); }

DAS_CORE_OCVWRAPPER_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto ToOcvType(DasImageFormat format) -> DAS::Utils::Expected<int>
{
    switch (format)
    {
    case DAS_IMAGE_FORMAT_RGB_888:
        return CV_8UC3;
    case DAS_IMAGE_FORMAT_RGBA_8888:
        [[fallthrough]];
    case DAS_IMAGE_FORMAT_RGBX_8888:
        return CV_8UC4;
    default:
        return tl::make_unexpected(DAS_E_INVALID_ENUM);
    }
}

DAS_NS_ANONYMOUS_DETAILS_END

IDasImageImpl::IDasImageImpl(
    int         height,
    int         width,
    int         type,
    void*       p_data,
    IDasMemory* p_das_data)
    : p_memory_{p_das_data}, mat_{height, width, type, p_data}
{
}

IDasImageImpl::IDasImageImpl(cv::Mat mat) : p_memory_{}, mat_{std::move(mat)} {}

DasResult IDasImageImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_out_object)
{
    return DAS::Utils::QueryInterface<IDasImage>(this, iid, pp_out_object);
}

DasResult IDasImageImpl::GetSize(DasSize* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size)

    p_out_size->width = mat_.cols;
    p_out_size->height = mat_.rows;

    return DAS_S_OK;
}

DasResult IDasImageImpl::GetChannelCount(int* p_out_channel_count)
{
    DAS_UTILS_CHECK_POINTER(p_out_channel_count)

    *p_out_channel_count = mat_.channels();

    return DAS_S_OK;
}

DasResult IDasImageImpl::Clip(const DasRect* p_rect, IDasImage** pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_rect)
    DAS_UTILS_CHECK_POINTER(pp_out_image)

    try
    {
        const auto& rect = *p_rect;
        const auto  clipped_mat = mat_(DAS::Core::OcvWrapper::ToMat(rect));
        auto        p_result = new IDasImageImpl{clipped_mat};
        p_result->p_memory_ = p_memory_;
        p_result->AddRef();
        *pp_out_image = p_result;
        return DAS_S_OK;
    }
    catch (std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult IDasImageImpl::GetDataSize(size_t* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size)

    size_t result = mat_.total();
    result *= mat_.elemSize1();
    *p_out_size = result;

    return DAS_S_OK;
}

DasResult IDasImageImpl::CopyTo(unsigned char* p_out_memory)
{
    DAS_UTILS_CHECK_POINTER(p_out_memory)

    try
    {
        size_t data_size;
        GetDataSize(&data_size);
        const auto int_data_size = static_cast<int>(data_size);
        mat_.copyTo({p_out_memory, int_data_size});
    }
    catch (cv::Exception& ex)
    {
        DAS_CORE_LOG_ERROR(ex.err);
        DAS_CORE_LOG_ERROR(
            "NOTE:\nfile = {}\nline = {}\nfunction = {}",
            ex.file,
            ex.line,
            ex.func);
    }

    return DAS_S_OK;
}

auto IDasImageImpl::GetImpl() -> cv::Mat { return mat_; }

DAS_CORE_OCVWRAPPER_NS_END

DasResult CreateIDasImageFromEncodedData(
    DasImageDesc* p_desc,
    IDasImage**   pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_desc)
    DAS_UTILS_CHECK_POINTER(pp_out_image)

    auto& [p_data, data_size, data_format] = *p_desc;

    const auto int_data_size = static_cast<int>(data_size);
    switch (data_format)
    {
    case DAS_IMAGE_FORMAT_JPG:
        [[fallthrough]];
    case DAS_IMAGE_FORMAT_PNG:
    {
        if (data_size == 0)
        {
            return DAS_E_INVALID_SIZE;
        }

        auto mat = cv::imdecode({p_data, int_data_size}, cv::IMREAD_UNCHANGED);
        cv::Mat rgb_mat{};
        cv::cvtColor(mat, rgb_mat, cv::COLOR_BGR2RGB);

        auto p_result = new DAS::Core::OcvWrapper::IDasImageImpl{rgb_mat};
        p_result->AddRef();
        *pp_out_image = p_result;
        return DAS_S_OK;
    }
    default:
        return DAS_E_INVALID_ENUM;
    }
}

DasResult CreateIDasImageFromDecodedData(
    const DasImageDesc* p_desc,
    const DasSize*      p_size,
    IDasImage**         pp_out_image)
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

    auto p_result = new DAS::Core::OcvWrapper::IDasImageImpl{owned_image};
    p_result->AddRef();
    *pp_out_image = p_result;

    return DAS_S_OK;
}

DasResult CreateIDasImageFromRgb888(
    IDasMemory*    p_alias_memory,
    const DasSize* p_size,
    IDasImage**    pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_alias_memory)
    DAS_UTILS_CHECK_POINTER(p_size)
    DAS_UTILS_CHECK_POINTER(pp_out_image)

    const auto&    size = *p_size;
    size_t         data_size;
    unsigned char* p_data;

    if (const auto get_size_result = p_alias_memory->GetSize(&data_size);
        DAS::IsFailed(get_size_result)) [[unlikely]]
    {
        return get_size_result;
    }

    if (const auto get_pointer_result = p_alias_memory->GetData(&p_data);
        DAS::IsFailed(get_pointer_result)) [[unlikely]]
    {
        return get_pointer_result;
    }

    const size_t required_size = std::abs(size.height * size.width * 4);
    if (required_size > data_size)
    {
        return DAS_E_OUT_OF_RANGE;
    }

    auto p_result = new DAS::Core::OcvWrapper::IDasImageImpl{
        size.height,
        size.width,
        CV_8UC4,
        p_data,
        p_alias_memory};
    p_result->AddRef();
    *pp_out_image = p_result;

    return DAS_S_OK;
}

DAS_NS_ANONYMOUS_DETAILS_BEGIN

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
                std::istream_iterator<char>{stream},
                std::istream_iterator<char>{},
                std::back_inserter(binary));
        });

    return cv::imdecode(binary, cv::IMREAD_COLOR);
}

DAS_NS_ANONYMOUS_DETAILS_END

DasResult DasPluginLoadImageFromResource(
    IDasTypeInfo*       p_type_info,
    IDasReadOnlyString* p_relative_path,
    IDasImage**         pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_type_info)
    DAS_UTILS_CHECK_POINTER(p_relative_path)
    DAS_UTILS_CHECK_POINTER(pp_out_image)

    const auto expected_storage =
        DAS::Core::ForeignInterfaceHost::g_plugin_manager
            .GetInterfaceStaticStorage(p_type_info);
    if (!expected_storage)
    {
        const auto error_code = expected_storage.error();
        DAS_CORE_LOG_ERROR(
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
        auto*      p_result = new Das::Core::OcvWrapper::IDasImageImpl{mat};
        *pp_out_image = p_result;
        p_result->AddRef();
        return DAS_S_OK;
    }
    catch (const std::ios_base::failure& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_ERROR(
            "Error happened when reading resource file. Error code = " DAS_STR(
                DAS_E_INVALID_FILE) ".");
        return DAS_E_INVALID_FILE;
    }
    catch (cv::Exception& ex)
    {
        DAS_CORE_LOG_ERROR(ex.err);
        DAS_CORE_LOG_ERROR(
            "NOTE:\nfile = {}\nline = {}\nfunction = {}",
            ex.file,
            ex.line,
            ex.func);
        return DAS_E_OPENCV_ERROR;
    }
}

DasRetImage DasPluginLoadImageFromResource(
    IDasSwigTypeInfo* p_type_info,
    DasReadOnlyString relative_path)
{
    DasRetImage            result{};
    DAS::DasPtr<IDasImage> p_image;

    Das::Core::ForeignInterfaceHost::SwigToCpp<IDasSwigTypeInfo> cpp_type_info{
        p_type_info};

    result.error_code = DasPluginLoadImageFromResource(
        &cpp_type_info,
        relative_path.Get(),
        p_image.Put());

    if (DAS::IsOk(result.error_code))
    {
        result.value = p_image;
    }

    return result;
}

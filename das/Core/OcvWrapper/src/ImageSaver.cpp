#include <das/Core/OcvWrapper/ImageSaver.h>

#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasMemory.h>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

DAS_DISABLE_WARNING_END

#include <cstring>
#include <fstream>
#include <new>
#include <optional>
#include <vector>

DAS_CORE_OCVWRAPPER_NS_BEGIN

namespace
{
    // DasImagePixelFormat -> OpenCV 通道类型；仅支持 GRAY/BGR/RGB/RGBA。
    std::optional<int> ToOcvMatType(ExportInterface::DasImagePixelFormat format)
    {
        switch (format)
        {
        case ExportInterface::DAS_PIXEL_FORMAT_GRAY:
            return CV_8UC1;
        case ExportInterface::DAS_PIXEL_FORMAT_BGR:
        case ExportInterface::DAS_PIXEL_FORMAT_RGB:
            return CV_8UC3;
        case ExportInterface::DAS_PIXEL_FORMAT_RGBA:
            return CV_8UC4;
        default:
            return std::nullopt;
        }
    }

    int ChannelCount(ExportInterface::DasImagePixelFormat format)
    {
        switch (format)
        {
        case ExportInterface::DAS_PIXEL_FORMAT_GRAY:
            return 1;
        case ExportInterface::DAS_PIXEL_FORMAT_BGR:
        case ExportInterface::DAS_PIXEL_FORMAT_RGB:
            return 3;
        case ExportInterface::DAS_PIXEL_FORMAT_RGBA:
            return 4;
        default:
            return 0;
        }
    }

    // JPG 编码前规范化：RGBA 去 alpha 转 3 通道 BGR；RGB 转 BGR（OpenCV
    // imencode 把 CV_8UC3 一律当作 BGR，直接构造会红蓝互换）。
    void NormalizeForJpg(
        const cv::Mat&                       mat,
        ExportInterface::DasImagePixelFormat pixel_format,
        cv::Mat&                             out)
    {
        switch (pixel_format)
        {
        case ExportInterface::DAS_PIXEL_FORMAT_RGBA:
            cv::cvtColor(mat, out, cv::COLOR_RGBA2BGR);
            return;
        case ExportInterface::DAS_PIXEL_FORMAT_RGB:
            cv::cvtColor(mat, out, cv::COLOR_RGB2BGR);
            return;
        default:
            out = mat;
            return;
        }
    }
} // namespace

DasResult ImageSaver::Encode(
    ExportInterface::IDasImage*         p_image,
    ExportInterface::DasImageFormat     format,
    int32_t                             quality,
    ExportInterface::IDasBinaryBuffer** pp_out_encoded)
{
    if (p_image == nullptr)
    {
        DAS_CORE_LOG_ERROR("ImageSaver::Encode: p_image is null");
        return DAS_E_INVALID_POINTER;
    }
    if (pp_out_encoded == nullptr)
    {
        DAS_CORE_LOG_ERROR("ImageSaver::Encode: pp_out_encoded is null");
        return DAS_E_INVALID_POINTER;
    }
    if (format != ExportInterface::DAS_IMAGE_FORMAT_JPG
        && format != ExportInterface::DAS_IMAGE_FORMAT_PNG)
    {
        DAS_CORE_LOG_ERROR(
            "ImageSaver::Encode: unsupported format={}",
            static_cast<int>(format));
        return DAS_E_INVALID_ENUM;
    }

    try
    {
        ExportInterface::DasSize size{};
        auto                     rc = p_image->GetSize(&size);
        if (DAS::IsFailed(rc))
        {
            return rc;
        }
        if (size.width <= 0 || size.height <= 0)
        {
            DAS_CORE_LOG_ERROR(
                "ImageSaver::Encode: invalid size {}x{}",
                size.width,
                size.height);
            return DAS_E_INVALID_SIZE;
        }

        ExportInterface::DasImagePixelFormat pixel_format =
            ExportInterface::DAS_PIXEL_FORMAT_UNKNOWN;
        rc = p_image->GetPixelFormat(&pixel_format);
        if (DAS::IsFailed(rc))
        {
            return rc;
        }
        const auto mat_type = ToOcvMatType(pixel_format);
        if (!mat_type)
        {
            DAS_CORE_LOG_ERROR(
                "ImageSaver::Encode: unsupported pixel_format={}",
                static_cast<int>(pixel_format));
            return DAS_E_INVALID_ENUM;
        }

        Das::DasPtr<ExportInterface::IDasBinaryBuffer> buffer;
        rc = p_image->GetBinaryBuffer(buffer.Put());
        if (DAS::IsFailed(rc))
        {
            return rc;
        }

        uint64_t data_size = 0;
        rc = buffer->GetSize(&data_size);
        if (DAS::IsFailed(rc))
        {
            return rc;
        }
        unsigned char* p_data = nullptr;
        rc = buffer->GetData(&p_data);
        if (DAS::IsFailed(rc))
        {
            return rc;
        }
        if (p_data == nullptr)
        {
            DAS_CORE_LOG_ERROR("ImageSaver::Encode: image data is null");
            return DAS_E_INVALID_POINTER;
        }

        const int64_t expected = static_cast<int64_t>(size.width)
                                 * static_cast<int64_t>(size.height)
                                 * ChannelCount(pixel_format);
        if (static_cast<int64_t>(data_size) != expected)
        {
            DAS_CORE_LOG_ERROR(
                "ImageSaver::Encode: data_size={} mismatch expected={}",
                data_size,
                expected);
            return DAS_E_INVALID_SIZE;
        }

        cv::Mat mat(
            size.height,
            size.width,
            *mat_type,
            static_cast<void*>(p_data));

        std::vector<unsigned char> encoded;
        std::vector<int>           params;
        std::string                ext;
        if (format == ExportInterface::DAS_IMAGE_FORMAT_JPG)
        {
            ext = ".jpg";
            params.push_back(cv::IMWRITE_JPEG_QUALITY);
            params.push_back(quality);
        }
        else
        {
            ext = ".png";
        }

        cv::Mat encode_mat;
        if (format == ExportInterface::DAS_IMAGE_FORMAT_JPG)
        {
            NormalizeForJpg(mat, pixel_format, encode_mat);
        }
        else
        {
            encode_mat = mat;
        }

        if (!cv::imencode(ext, encode_mat, encoded, params))
        {
            DAS_CORE_LOG_ERROR("ImageSaver::Encode: imencode failed");
            return DAS_E_OPENCV_ERROR;
        }

        Das::DasOutPtr<ExportInterface::IDasBinaryBuffer> out(pp_out_encoded);
        Das::DasPtr<ExportInterface::IDasMemory>          memory;
        rc = ::CreateIDasMemory(encoded.size(), memory.Put());
        if (DAS::IsFailed(rc))
        {
            return rc;
        }
        Das::DasPtr<ExportInterface::IDasBinaryBuffer> binary;
        rc = memory->GetBinaryBuffer(0, binary.Put());
        if (DAS::IsFailed(rc))
        {
            return rc;
        }
        unsigned char* p_dst = nullptr;
        rc = binary->GetData(&p_dst);
        if (DAS::IsFailed(rc))
        {
            return rc;
        }
        if (p_dst == nullptr)
        {
            DAS_CORE_LOG_ERROR(
                "ImageSaver::Encode: output buffer data is null");
            return DAS_E_INVALID_POINTER;
        }
        std::memcpy(p_dst, encoded.data(), encoded.size());

        out.Set(binary.Get());
        out.Keep();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("ImageSaver::Encode: out of memory");
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (const cv::Exception& ex)
    {
        DAS_CORE_LOG_ERROR(
            "ImageSaver::Encode: OpenCV exception: {}",
            ex.what());
        return DAS_E_OPENCV_ERROR;
    }
}

DasResult ImageSaver::SaveToFile(
    ExportInterface::IDasImage*     p_image,
    ExportInterface::DasImageFormat format,
    int32_t                         quality,
    const std::filesystem::path&    path)
{
    Das::DasPtr<ExportInterface::IDasBinaryBuffer> encoded;
    auto rc = Encode(p_image, format, quality, encoded.Put());
    if (DAS::IsFailed(rc))
    {
        return rc;
    }

    uint64_t size = 0;
    rc = encoded->GetSize(&size);
    if (DAS::IsFailed(rc))
    {
        return rc;
    }
    unsigned char* p_data = nullptr;
    rc = encoded->GetData(&p_data);
    if (DAS::IsFailed(rc))
    {
        return rc;
    }
    if (p_data == nullptr)
    {
        DAS_CORE_LOG_ERROR("ImageSaver::SaveToFile: encoded data is null");
        return DAS_E_INVALID_POINTER;
    }

    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
    {
        DAS_CORE_LOG_ERROR(
            "ImageSaver::SaveToFile: cannot open {}",
            path.string());
        return DAS_E_INVALID_FILE;
    }
    output.write(
        reinterpret_cast<const char*>(p_data),
        static_cast<std::streamsize>(size));
    output.flush();
    if (!output)
    {
        DAS_CORE_LOG_ERROR(
            "ImageSaver::SaveToFile: write failed for {}",
            path.string());
        return DAS_E_INVALID_FILE;
    }
    return DAS_S_OK;
}

DAS_CORE_OCVWRAPPER_NS_END

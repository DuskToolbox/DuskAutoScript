#include <das/DasConfig.h>
#include <das/_autogen/idl/abi/IDasMemory.h>

#if defined(_WIN32) || defined(__CYGWIN__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // WIN32_LEAN_AND_MEAN

#include <Windows.h>
#endif // WIN32

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_UNUSED_PARAMETER

DAS_DISABLE_WARNING_END

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4068)
#endif
#include <cstddef>
#include <gzip/decompress.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "AdbCaptureImpl.h"
#include "ErrorLensImpl.h"
#include "PluginImpl.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100) // Unreferenced parameter in boost::process::v2
#endif

#include <array>
#include <boost/asio.hpp>
#include <boost/pfr.hpp>
#include <boost/process/v2/execute.hpp>
#include <boost/process/v2/process.hpp>
#include <das/DasApi.h>
#include <das/DasException.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/DasLogger.h>
#include <das/_autogen/idl/abi/IDasImage.h>
#include <sstream>
#include <system_error>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

DAS_NS_BEGIN

/**
 * @brief reference from
 *  <a
 * href="https://developer.android.com/reference/android/graphics/PixelFormat">PixelFormat</a>
 *  <a
 * href="https://android.googlesource.com/platform/frameworks/base/+/android-4.3_r2.3/cmds/screencap/screencap.cpp">screencap.cpp
 * in Android 4.23</a> <a
 * href="https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android-s-beta-4/cmds/screencap/screencap.cpp">screencap.cpp
 * in Android S Beta 4</a> \n NOTE: kN32_SkColorType selects native 32-bit
 * ARGB format.\n On little endian processors, pixels containing 8-bit ARGB
 * components pack into 32-bit kBGRA_8888_SkColorType.\n On big endian
 * processors, pixels pack into 32-bit kRGBA_8888_SkColorType.\n In this plugin,
 * we assume kN32_SkColorType is RGBA_8888.
 */
enum class AdbCaptureFormat : uint32_t
{
    RGBA_8888 = 1,
    RGBX_8888 = 2,
    RGB_888 = 3,
    RGB_565 = 4
};

constexpr std::size_t ADB_CAPTURE_HEADER_SIZE = 16;

struct AdbCaptureHeader
{
    uint32_t h;
    uint32_t w;
    uint32_t f;
};

AdbCapture::AdbCapture(
    const std::filesystem::path& adb_path,
    std::string_view             adb_device_serial)
    : capture_png_command_{DAS::fmt::format(
          "{} -s {} exec-out screencap -p",
          adb_path.string(),
          adb_device_serial)},
      capture_gzip_raw_command_{DAS::fmt::format(
          R"({} -s {} exec-out "screencap | gzip -1")",
          adb_path.string(),
          adb_device_serial)},
      get_screen_size_command_{DAS::fmt::format(
          R"({} -s {} shell dumpsys window displays | grep -o -E cur=+[^\\ ]+ | grep -o -E [0-9]+)",
          adb_path.string(),
          adb_device_serial)}
{
}

AdbCapture::~AdbCapture() = default;

DAS_NS_ANONYMOUS_DETAILS_BEGIN

constexpr uint32_t PROCESS_TIMEOUT_IN_S = 10;

std::size_t ComputeScreenshotSize(
    const std::int32_t width,
    const std::int32_t height) noexcept
{
    // header + data (assume 32bit color)
    return ADB_CAPTURE_HEADER_SIZE
           + static_cast<std::size_t>(width * height * 4);
}

// DasMemoryImpl - 包装类，提供便捷的 C++ API
class DasMemoryImpl
{
    DasPtr<ExportInterface::IDasMemory> p_data_;

public:
    DasMemoryImpl(size_t size_in_bytes)
    {
        ::CreateIDasMemory(size_in_bytes, p_data_.Put());
    }

    ~DasMemoryImpl() = default;

    unsigned char* GetData() const
    {
        DasPtr<ExportInterface::IDasBinaryBuffer> p_binary_buffer{};
        DAS_THROW_IF_FAILED(p_data_->GetBinaryBuffer(p_binary_buffer.Put()));
        unsigned char* p_data{nullptr};
        DAS_THROW_IF_FAILED(p_binary_buffer->GetData(&p_data));
        return p_data;
    }

    size_t GetSize() const
    {
        size_t size{};
        p_data_->GetSize(&size);
        return size;
    }

    void SetOffset(const int64_t offset) { p_data_->SetOffset(offset); }

    // stl-like api
    unsigned char& operator[](size_t size_in_bytes)
    {
        return *(GetData() + size_in_bytes);
    }

    void resize(size_t new_size)
    {
        if (new_size > GetSize())
        {
            ::CreateIDasMemory(new_size, p_data_.Put());
        }
    }

    ExportInterface::IDasMemory* GetImpl() const noexcept
    {
        return p_data_.Get();
    }
};

template <class Buffer>
struct CommandExecutorContext : public DAS::Utils::NonCopyableAndNonMovable
{
private:
    boost::asio::io_context          ioc_;
    boost::asio::steady_timer        timeout_timer_;
    boost::asio::cancellation_signal sig_;
    std::chrono::milliseconds        timeout_in_ms_;
    std::string                      command_;
    DasResult                        result_;
    Buffer                           buffer_;

public:
    CommandExecutorContext(
        std::string_view    command,
        const std::uint32_t timeout)
        : ioc_{}, timeout_timer_{ioc_, std::chrono::seconds(timeout)}, sig_{},
          timeout_in_ms_{timeout * 1000}, command_{command},
          result_{DAS_E_UNDEFINED_RETURN_VALUE}, buffer_{}
    {
        boost::process::v2::async_execute(
            boost::process::v2::process{ioc_, command_, {}},
            boost::asio::bind_cancellation_slot(
                sig_.slot(),
                [this](boost::system::error_code ec, int exit_code)
                {
                    const auto info =
                        DAS::fmt::format("{} return {}.", command_, exit_code);
                    if (ec)
                    {
                        DAS_LOG_ERROR(info.c_str());
                        const auto error_message = DAS::fmt::format(
                            "Error happened when executing command {}. Message = {}.",
                            command_,
                            ec.message());
                        DAS_LOG_ERROR(error_message.c_str());
                        if (result_ != DAS_E_TIMEOUT)
                        {
                            result_ = DAS_E_INTERNAL_FATAL_ERROR;
                        }
                        return;
                    }
                    else [[likely]]
                    {
                        DAS_LOG_INFO(info.c_str());
                        result_ = DAS_S_OK;
                    }

                    timeout_timer_.cancel(); // we're done earlier
                }));

        timeout_timer_.async_wait(
            [this](boost::system::error_code ec)
            {
                if (ec) // we were cancelled, do nothing
                {
                    return;
                }
                result_ = DAS_E_TIMEOUT;
                const auto error_message = DAS::fmt::format(
                    "Timeout detected when executing command {}.",
                    command_);
                DAS_LOG_ERROR(error_message.c_str());
                sig_.emit(boost::asio::cancellation_type::partial);
                // request exit first, but terminate after another
                // timeout_in_ms_
                timeout_timer_.expires_after(this->timeout_in_ms_);
                timeout_timer_.async_wait(
                    [this](boost::system::error_code timer_ec)
                    {
                        if (!timer_ec)
                        {
                            sig_.emit(boost::asio::cancellation_type::terminal);
                        }
                    });
            });
    }

    DasResult Run()
    {
        ioc_.run();
        return result_;
    }

    const Buffer& GetBuffer() const { return buffer_; }
    Buffer&       GetBuffer() { return buffer_; }
};

AdbCaptureHeader ResolveHeader(const char* p_header)
{
    AdbCaptureHeader header;
    std::memcpy(&header, p_header, sizeof(AdbCaptureHeader));
    return header;
}

auto ComputeDataSizeFromHeader(const AdbCaptureHeader header)
    -> DAS::Utils::Expected<std::size_t>
{
    switch (static_cast<AdbCaptureFormat>(header.f))
    {
    case AdbCaptureFormat::RGBA_8888:
        [[fallthrough]];
    case AdbCaptureFormat::RGBX_8888:
        [[fallthrough]];
    case AdbCaptureFormat::RGB_888:
        return header.w * header.h * 4;
    // RGB_565 and so on.
    default:
        const auto error_message =
            DAS::fmt::format("Unsupported color format: {}", header.f);
        DAS_LOG_ERROR(error_message.c_str());
        return tl::make_unexpected(UNSUPPORTED_COLOR_FORMAT);
    }
}

DAS::Utils::Expected<ExportInterface::DasImageFormat> Convert(
    const AdbCaptureFormat format)
{
    switch (format)
    {
        using enum AdbCaptureFormat;
    case RGBA_8888:
        [[likely]] return ExportInterface::DAS_IMAGE_FORMAT_RGBA_8888;
    case RGBX_8888:
        return ExportInterface::DAS_IMAGE_FORMAT_RGBX_8888;
    case RGB_888:
        return ExportInterface::DAS_IMAGE_FORMAT_RGB_888;
    default:
        return tl::make_unexpected(UNSUPPORTED_COLOR_FORMAT);
    }
}

DAS_NS_ANONYMOUS_DETAILS_END

/**
 *
 * @tparam T buffer type
 * @param command
 * @param timeout timeout in seconds.
 * @return a CommandExecutorContext object.
 */
template <class T>
auto MakeCommandExecutorContext(
    std::string_view    command,
    const std::uint32_t timeout)
{
    T tmp_buffer{};
    return Details::CommandExecutorContext<T>{
        command,
        timeout,
        std::move(tmp_buffer)};
}

DAS::Utils::Expected<AdbCapture::Size> AdbCapture::GetDeviceSize() const
{
    Details::CommandExecutorContext<std::string> context{
        get_screen_size_command_,
        Details::PROCESS_TIMEOUT_IN_S};
    const auto result_code = context.Run();
    if (!IsOk(result_code))
    {
        const auto error_message = DAS::fmt::format(
            "Failed to execute command: {}. Error code: {}.",
            get_screen_size_command_,
            result_code);
        DAS_LOG_ERROR(error_message.c_str());
        return tl::make_unexpected(result_code);
    }
    Size              result{};
    std::stringstream output_string_stream{context.GetBuffer()};
    int               size_1{0};
    int               size_2{0};
    output_string_stream >> size_1 >> size_2;

    if (size_1 == 0 || size_2 == 0)
    {
        const auto error_message = DAS::fmt::format(
            "Unexpected error when getting screen size. Received output: {}",
            context.GetBuffer());
        DAS_LOG_ERROR(error_message.c_str());
        // todo return tl::make_unexpected();
    }

    result.width = (std::max)(size_1, size_2);
    result.height = (std::min)(size_1, size_2);
    return result;
}

DasResult AdbCapture::CaptureRawWithGZip()
{
    DasResult result{DAS_S_OK};
    // Initialize buffer.
    auto adb_output_buffer = DAS::Utils::MakeContainerOfSize<std::vector<char>>(
        Details::ComputeScreenshotSize(
            adb_device_screen_size_.width,
            adb_device_screen_size_.height));
    // Run adb and receive screen capture.
    Details::CommandExecutorContext<decltype(adb_output_buffer)> context{
        capture_gzip_raw_command_,
        Details::PROCESS_TIMEOUT_IN_S};
    // Initialize the objects that need to be used later.
    auto decompressed_data = Details::DasMemoryImpl(
        Details::ComputeScreenshotSize(
            adb_device_screen_size_.width,
            adb_device_screen_size_.height));
    const gzip::Decompressor decompressor{};
    // wait for the process to exit.
    const auto exec_result = context.Run();
    if (!IsOk(exec_result))
    {
        return exec_result;
    }

    decompressor.decompress(
        decompressed_data,
        context.GetBuffer().data(),
        context.GetBuffer().size());

    DasPtr<ExportInterface::IDasBinaryBuffer> p_decompressed_binary_buffer{};
    DAS_THROW_IF_FAILED(decompressed_data.GetImpl()->GetBinaryBuffer(
        p_decompressed_binary_buffer.Put()));
    unsigned char* p_decompressed_data = nullptr;
    DAS_THROW_IF_FAILED(
        p_decompressed_binary_buffer->GetData(&p_decompressed_data));
    const auto header =
        Details::ResolveHeader(reinterpret_cast<char*>(p_decompressed_data));
    Details::ComputeDataSizeFromHeader(header)
        .and_then(
            [&decompressed_data, &header](const std::size_t expected_data_size)
                -> DAS::Utils::Expected<void>
            {
                const auto decompressed_data_size = decompressed_data.GetSize();
                if (expected_data_size > decompressed_data_size) [[unlikely]]
                {
                    const auto error_message = DAS::fmt::format(
                        "Received unexpected data size.\n Expected data size: {}.\n Received data size: {}.\n Data format: {}.",
                        expected_data_size,
                        decompressed_data_size,
                        header.f);
                    DAS_LOG_ERROR(error_message.c_str());
                    return tl::make_unexpected(CAPTURE_DATA_TOO_LESS);
                }
                return {};
            })
        .and_then(
            [&header]
            {
                return Details::Convert(
                    static_cast<AdbCaptureFormat>(header.f));
            })
        .and_then(
            [&decompressed_data,
             &header](const ExportInterface::DasImageFormat color_format)
                -> DAS::Utils::Expected<void>
            {
                ExportInterface::DasSize size{
                    static_cast<int32_t>(header.w),
                    static_cast<int32_t>(header.h)};
                decompressed_data.SetOffset(ADB_CAPTURE_HEADER_SIZE);

                // 格式符合预期则直接避免拷贝
                if (color_format == ExportInterface::DAS_IMAGE_FORMAT_RGB_888)
                {
                    DasPtr<ExportInterface::IDasImage> p_image{};
                    const auto                         create_image_result =
                        ::CreateIDasImageFromRgb888(
                            decompressed_data.GetImpl(),
                            &size,
                            p_image.Put());
                    if (IsOk(create_image_result)) [[likely]]
                    {
                        return {};
                    }
                    return tl::make_unexpected(create_image_result);
                }

                DasPtr<ExportInterface::IDasBinaryBuffer>
                    p_desc_binary_buffer{};
                DAS_THROW_IF_FAILED(
                    decompressed_data.GetImpl()->GetBinaryBuffer(
                        p_desc_binary_buffer.Put()));
                unsigned char* p_decompressed_data_for_desc = nullptr;
                DAS_THROW_IF_FAILED(p_desc_binary_buffer->GetData(
                    &p_decompressed_data_for_desc));
                DasImageDesc desc{
                    .p_data =
                        reinterpret_cast<char*>(p_decompressed_data_for_desc),
                    .data_size =
                        decompressed_data.GetSize() - ADB_CAPTURE_HEADER_SIZE,
                    .data_format = color_format};

                DasPtr<ExportInterface::IDasImage> p_image{};
                const auto                         create_image_result =
                    ::CreateIDasImageFromDecodedData(
                        &desc,
                        &size,
                        p_image.Put());
                if (IsOk(create_image_result)) [[likely]]
                {
                    return {};
                }
                return tl::make_unexpected(create_image_result);
            })
        .or_else([&result](const auto error_code) { result = error_code; });
    return result;
}

DasResult AdbCapture::CaptureRaw() { return DAS_E_NO_IMPLEMENTATION; }

DasResult AdbCapture::CapturePng() { return DAS_E_NO_IMPLEMENTATION; }

DasResult AdbCapture::CaptureRawByNc() { return DAS_E_NO_IMPLEMENTATION; }

auto AdbCapture::AutoDetectType()
    -> DAS::Utils::Expected<DasResult (AdbCapture::*)()>
{
    if (current_capture_method != nullptr)
    {
        return {};
    }
    DAS_LOG_INFO("Detecting fastest adb capture way.");
    DasResult result{DAS_S_OK};
    // TODO: Check more capture methods.
    if (result = CaptureRawWithGZip(); IsOk(result)) [[likely]]
    {
        current_capture_method = &AdbCapture::CaptureRawWithGZip;
    }
    if (IsOk(result))
    {
        return &AdbCapture::CaptureRawWithGZip;
    }

    return tl::make_unexpected(result);
}

DasResult AdbCapture::Capture(ExportInterface::IDasImage** pp_out_image)
{
    (void)pp_out_image;
    DasResult result{DAS_S_OK};
    if (boost::pfr::eq(adb_device_screen_size_, Size{0, 0})) [[unlikely]]
    {
        GetDeviceSize()
            .or_else([&result](const auto error_code) { result = error_code; })
            .map([&ref_size = this->adb_device_screen_size_](Size size)
                 { ref_size = size; });
        if (!IsOk(result)) [[unlikely]]
        {
            return result;
        }
    }
    if (current_capture_method == nullptr) [[unlikely]]
    {
        AutoDetectType()
            .or_else([&result](const auto error_code) { result = error_code; })
            .map([&this_current_capture_method =
                      this->current_capture_method](const auto pointer)
                 { this_current_capture_method = pointer; });
    }
    return DAS_E_NO_IMPLEMENTATION;
}

DAS_NS_END
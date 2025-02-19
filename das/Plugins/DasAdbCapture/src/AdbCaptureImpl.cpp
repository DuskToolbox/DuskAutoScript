#include <das/DasConfig.h>
#include <das/ExportInterface/IDasMemory.h>

#if defined(_WIN32) || defined(__CYGWIN__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // WIN32_LEAN_AND_MEAN

#include <Windows.h>
#endif // WIN32

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_UNUSED_PARAMETER

#include <boost/process/async.hpp>

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
#include <das/ExportInterface/DasLogger.h>
#include <das/ExportInterface/IDasImage.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/QueryInterface.hpp>
#include <das/Utils/fmt.h>
#include "PluginImpl.h"
#include <array>
#include <boost/asio.hpp>
#include <boost/pfr.hpp>
#include <boost/process.hpp>
#include <boost/process/async_pipe.hpp>
#include <sstream>
#include <system_error>

DAS_NS_BEGIN

/**
 * @brief reference from
 *  <a
 * href="https://developer.android.com/reference/android/graphics/PixelFormat">PixelFormat</a>
 *  <a
 * href="https://android.googlesource.com/platform/frameworks/base/+/android-4.3_r2.3/cmds/screencap/screencap.cpp">screencap.cpp
 * in Android 4.23</a> <a
 * href="https://android.googlesource.com/platform/frameworks/base/+/refs/heads/android-s-beta-4/cmds/screencap/screencap.cpp">screencap.cpp
 * in Android S Beta 4</a> \n NOTE: kN32_SkColorType selects the native 32-bit
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
//   get_nc_address_command_{DAS::fmt::format(
//       R"({} -s {} shell " cat /proc/net/arp | grep : ")",
//       adb_path.string(),
//       adb_device_serial)}
//   capture_raw_by_nc_command_{DAS::fmt::format(
//       R"({} -s {} exec-out "screencap | nc -w 3 {} {}")",
//       adb_path.string(),
//       adb_device_serial,
//       nc_address_,
//       nc_port_)},
{
    DAS::AdbCaptureAddRef();
}

AdbCapture::~AdbCapture() { DAS::AdbCaptureRelease(); }

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

template <class Buffer>
struct CommandExecutorContext : public DAS::Utils::NonCopyableAndNonMovable
{
private:
    void OnProcessExited(int exit_code, const std::error_code& error_code)
    {
        timer.cancel();

        if (exit_code || error_code)
        {
            constexpr const auto& string_template =
                R"(Executing command: "{}".\nExit code: {}.\nError code: {}. Message: "{}".)";
            const auto error_code_message = error_code.message();
            const auto error_message = DAS::fmt::format(
                string_template,
                this->command,
                exit_code,
                error_code.value(),
                error_code_message);
            DasLogErrorU8(error_message.c_str());
        }
        else
        {
            constexpr const auto& string_template =
                R"(Executing command: "{}" successfully.)";
            const auto error_message =
                DAS::fmt::format(string_template, this->command);
            DasLogInfoU8(error_message.c_str());
        }
    }

    boost::asio::awaitable<void> WaitProcessTimeout()
    {
        while (true)
        {
            const auto [error_code] = co_await timer.async_wait(
                boost::asio::as_tuple(boost::asio::use_awaitable));
            if (error_code != boost::asio::error::operation_aborted) [[likely]]
            {
                break;
            }
            else if (error_code == boost::system::errc::success)
            {
                process.terminate();
                process_out.close();
                const auto error_message = DAS::fmt::format(
                    R"(Process timeout ({}s has been waiting).\n Error code: {}. Message: "{}".)",
                    timeout,
                    error_code.value(),
                    error_code.message());
                DasLogErrorU8(error_message.c_str());
            }
            else
            {
                const auto error_message = DAS::fmt::format(
                    R"(Unexpected error when waiting timeout.\n Error code: {}. Message: "{}".)",
                    error_code.value(),
                    error_code.message());
                DasLogErrorU8(error_message.c_str());
            }
        }
    }

public:
    template <class T>
    CommandExecutorContext(
        std::string_view    command,
        const std::uint32_t timeout,
        T&&                 buffer)
        requires(!std::is_lvalue_reference_v<T>)
        : buffer{std::forward<T>(buffer)},
          process{
              command.data(),
              boost::process::std_out > process_out,
              ioc,
              boost::process::on_exit(
                  [this](int exit_code, const std::error_code& error_code)
                  { this->OnProcessExited(exit_code, error_code); })},
          timer{ioc, std::chrono::seconds(timeout)}, timeout{timeout},
          command{command}
    {
        // 超时
        boost::asio::co_spawn(
            ioc,
            [this]() { return this->WaitProcessTimeout(); }(),
            boost::asio::detached);
        // 读取输出
        boost::asio::co_spawn(
            ioc,
            [this]() -> boost::asio::awaitable<void>
            {
                const auto [error_code, read_length] =
                    co_await boost::asio::async_read(
                        process_out,
                        boost::asio::dynamic_buffer(this->buffer),
                        boost::asio::as_tuple(boost::asio::use_awaitable));
                if (!error_code) [[unlikely]]
                {
                    const auto error_message = DAS::fmt::format(
                        "Unexpected error when reading stdout for command: {}.\n Error code: {}. Message: {}",
                        this->command.c_str(),
                        error_code.value(),
                        error_code.message());
                    DAS_LOG_ERROR(error_message.c_str());
                }
            }(),
            [](std::exception_ptr p_ex)
            {
                try
                {
                    if (p_ex)
                    {
                        std::rethrow_exception(p_ex);
                    }
                }
                catch (const std::runtime_error& ex)
                {
                    DasLogErrorU8(ex.what());
                }
            });
    }
    Buffer                     buffer;
    boost::asio::io_context    ioc{};
    boost::process::async_pipe process_out{ioc};
    boost::process::child      process;
    boost::asio::steady_timer  timer;
    std::uint32_t              timeout;
    std::string                command;
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

DAS::Utils::Expected<DasImageFormat> Convert(const AdbCaptureFormat format)
{
    switch (format)
    {
        using enum AdbCaptureFormat;
    case RGBA_8888:
        [[likely]] return DAS_IMAGE_FORMAT_RGBA_8888;
    case RGBX_8888:
        return DAS_IMAGE_FORMAT_RGBX_8888;
    case RGB_888:
        return DAS_IMAGE_FORMAT_RGB_888;
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
    auto context = MakeCommandExecutorContext<std::string>(
        get_screen_size_command_,
        Details::PROCESS_TIMEOUT_IN_S);
    Size result{};
    context.ioc.run();
    std::stringstream output_string_stream{context.buffer};
    int               size_1{0};
    int               size_2{0};
    output_string_stream >> size_1 >> size_2;

    if (size_1 == 0 || size_2 == 0)
    {
        const auto error_message = DAS::fmt::format(
            "Unexpected error when getting screen size. Received output: {}",
            context.buffer);
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
        Details::PROCESS_TIMEOUT_IN_S,
        std::move(adb_output_buffer)};
    // Initialize the objects that need to be used later.
    auto decompressed_data = DasMemory(Details::ComputeScreenshotSize(
        adb_device_screen_size_.width,
        adb_device_screen_size_.height));
    const gzip::Decompressor decompressor{};
    // wait for the process to exit.
    context.ioc.run();

    decompressor.decompress(
        decompressed_data,
        context.buffer.data(),
        context.buffer.size());

    const auto header = Details::ResolveHeader(
        reinterpret_cast<char*>(decompressed_data.GetData()));
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
            [&header] {
                return Details::Convert(
                    static_cast<AdbCaptureFormat>(header.f));
            })
        .and_then(
            [&decompressed_data, &header](
                const DasImageFormat color_format) -> DAS::Utils::Expected<void>
            {
                DasSize size{
                    static_cast<int32_t>(header.w),
                    static_cast<int32_t>(header.h)};
                decompressed_data.SetBeginOffset(ADB_CAPTURE_HEADER_SIZE);

                // 格式符合预期则直接避免拷贝
                if (color_format == DAS_IMAGE_FORMAT_RGB_888)
                {
                    DasPtr<IDasImage> p_image{};
                    const auto        create_image_result =
                        ::CreateIDasImageFromRgb888(
                            decompressed_data.Get(),
                            &size,
                            p_image.Put());
                    if (IsOk(create_image_result)) [[likely]]
                    {
                        return {};
                    }
                    return tl::make_unexpected(create_image_result);
                }

                DasImageDesc desc{
                    .p_data =
                        reinterpret_cast<char*>(decompressed_data.GetData()),
                    .data_size =
                        decompressed_data.GetSize() - ADB_CAPTURE_HEADER_SIZE,
                    .data_format = color_format};

                DasPtr<IDasImage> p_image{};
                const auto        create_image_result =
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

int64_t AdbCapture::AddRef() { return ref_counter_.AddRef(); }

int64_t AdbCapture::Release() { return ref_counter_.Release(this); }

DasResult AdbCapture::QueryInterface(const DasGuid& iid, void** pp_object)
{
    return DAS::Utils::QueryInterface<IDasCapture>(this, iid, pp_object);
}

DAS_IMPL AdbCapture::GetGuid(DasGuid* p_out_guid)
{
    if (p_out_guid == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    *p_out_guid = DasIidOf<AdbCapture>();

    return DAS_S_OK;
}

DasResult AdbCapture::GetRuntimeClassName(
    IDasReadOnlyString** pp_out_class_name)
{
    DAS_UTILS_GET_RUNTIME_CLASS_NAME_IMPL(Das::AdbCapture, pp_out_class_name);
}

DasResult AdbCapture::Capture(IDasImage** pp_out_image)
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
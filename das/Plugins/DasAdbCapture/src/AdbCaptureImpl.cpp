#include <das/DasConfig.h>
#include <das/_autogen/idl/abi/IDasMemory.h>

#if defined(_WIN32) || defined(__CYGWIN__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // WIN32_LEAN_AND_MEAN

#include <Windows.h>
#endif // WIN32

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <zlib.h>

#include "AdbCaptureImpl.h"
#include "ErrorLensImpl.h"
#include "PluginImpl.h"

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_PROCESS_WARNING

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/pfr.hpp>
#include <boost/process/v2/execute.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <chrono>
#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/DasLogger.h>
#include <das/_autogen/idl/abi/IDasImage.h>
#include <iterator>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>

DAS_DISABLE_WARNING_END

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
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t dataspace;
};

static_assert(sizeof(AdbCaptureHeader) == ADB_CAPTURE_HEADER_SIZE);
static_assert(offsetof(AdbCaptureHeader, width) == 0);
static_assert(offsetof(AdbCaptureHeader, height) == 4);
static_assert(offsetof(AdbCaptureHeader, format) == 8);
static_assert(offsetof(AdbCaptureHeader, dataspace) == 12);

AdbCapture::AdbCapture(
    const std::filesystem::path& adb_path,
    std::string_view             adb_device_serial)
    : capture_png_command_{DAS::fmt::format(
          "{} -s {} exec-out screencap -p",
          DAS::Utils::U8AsString(adb_path.u8string()),
          adb_device_serial)},
      capture_gzip_raw_command_{DAS::fmt::format(
          R"({} -s {} exec-out "screencap | gzip -1")",
          DAS::Utils::U8AsString(adb_path.u8string()),
          adb_device_serial)},
      get_screen_size_command_{DAS::fmt::format(
          R"({} -s {} shell dumpsys window displays | grep -o -E cur=+[^\\ ]+ | grep -o -E [0-9]+)",
          DAS::Utils::U8AsString(adb_path.u8string()),
          adb_device_serial)}
{
}

AdbCapture::~AdbCapture() = default;

DAS_NS_ANONYMOUS_DETAILS_BEGIN

constexpr uint32_t PROCESS_TIMEOUT_IN_S = 10;

enum class InflateMode
{
    StopWhenOutputFull,
    RequireExactOutputSize
};

class InflateStream final : public DAS::Utils::NonCopyableAndNonMovable
{
public:
    InflateStream() = default;

    DasResult Init()
    {
        if (const auto result = inflateInit2(&stream_, 15 + 32); result != Z_OK)
            [[unlikely]]
        {
            const auto error_message =
                DAS::fmt::format("inflateInit2 failed, result={}.", result);
            DAS_LOG_ERROR(error_message.c_str());
            return DAS_E_INTERNAL_FATAL_ERROR;
        }
        initialized_ = true;
        return DAS_S_OK;
    }

    ~InflateStream()
    {
        if (initialized_)
        {
            inflateEnd(&stream_);
        }
    }

    z_stream& Get() noexcept { return stream_; }

private:
    z_stream stream_{};
    bool     initialized_{false};
};

class PayloadMemoryView final : public ExportInterface::IDasMemory
{
public:
    explicit PayloadMemoryView(
        ExportInterface::IDasBinaryBuffer* p_payload_buffer)
        : payload_buffer_{p_payload_buffer}
    {
    }

    uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

    uint32_t DAS_STD_CALL Release() override
    {
        const auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out_object) override
    {
        if (pp_out_object == nullptr) [[unlikely]]
        {
            return DAS_E_INVALID_POINTER;
        }
        if (iid == DasIidOf<IDasBase>())
        {
            *pp_out_object = static_cast<IDasBase*>(this);
            AddRef();
            return DAS_S_OK;
        }
        if (iid == DasIidOf<ExportInterface::IDasMemory>())
        {
            *pp_out_object = static_cast<ExportInterface::IDasMemory*>(this);
            AddRef();
            return DAS_S_OK;
        }
        *pp_out_object = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    DasResult GetBinaryBuffer(
        uint64_t                            offset,
        ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
    {
        return GetView(offset, pp_out_buffer);
    }

    DasResult GetMutableView(
        uint64_t                            offset,
        ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
    {
        return GetView(offset, pp_out_buffer);
    }

    DasResult GetSize(uint64_t* p_out_size) override
    {
        if (p_out_size == nullptr) [[unlikely]]
        {
            return DAS_E_INVALID_POINTER;
        }
        if (!payload_buffer_) [[unlikely]]
        {
            return DAS_E_INVALID_POINTER;
        }
        return payload_buffer_->GetSize(p_out_size);
    }

private:
    DasResult GetView(
        uint64_t                            offset,
        ExportInterface::IDasBinaryBuffer** pp_out_buffer)
    {
        if (pp_out_buffer == nullptr) [[unlikely]]
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_buffer = nullptr;
        if (!payload_buffer_) [[unlikely]]
        {
            return DAS_E_INVALID_POINTER;
        }

        uint64_t payload_size = 0;
        if (const auto result = payload_buffer_->GetSize(&payload_size);
            IsFailed(result)) [[unlikely]]
        {
            return result;
        }

        if (offset > payload_size) [[unlikely]]
        {
            const auto error_message = DAS::fmt::format(
                "PayloadMemoryView offset out of range: offset={}, size={}.",
                offset,
                payload_size);
            DAS_LOG_ERROR(error_message.c_str());
            return DAS_E_OUT_OF_RANGE;
        }

        if (offset != 0) [[unlikely]]
        {
            const auto error_message = DAS::fmt::format(
                "PayloadMemoryView only supports offset 0, got offset={}.",
                offset);
            DAS_LOG_ERROR(error_message.c_str());
            return DAS_E_NO_IMPLEMENTATION;
        }

        return payload_buffer_->QueryInterface(
            DasIidOf<ExportInterface::IDasBinaryBuffer>(),
            reinterpret_cast<void**>(pp_out_buffer));
    }

    std::atomic<uint32_t>                     ref_count_{0};
    DasPtr<ExportInterface::IDasBinaryBuffer> payload_buffer_;
};

bool TryMultiplyUint64(
    const uint64_t lhs,
    const uint64_t rhs,
    uint64_t*      p_out_value) noexcept
{
    if (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs)
    {
        return false;
    }
    *p_out_value = lhs * rhs;
    return true;
}

DasResult InflateGzipToBuffer(
    const char*    p_compressed_data,
    std::size_t    compressed_size,
    unsigned char* p_output_data,
    std::size_t    output_size,
    InflateMode    mode,
    std::size_t*   p_out_bytes_written)
{
    if (p_out_bytes_written == nullptr) [[unlikely]]
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_bytes_written = 0;
    if (p_compressed_data == nullptr || compressed_size == 0) [[unlikely]]
    {
        DAS_LOG_ERROR("InflateGzipToBuffer received empty gzip input.");
        return CAPTURE_DATA_TOO_LESS;
    }
    if (p_output_data == nullptr && output_size != 0) [[unlikely]]
    {
        return DAS_E_INVALID_POINTER;
    }

    InflateStream inflate_stream;
    if (const auto init_result = inflate_stream.Init(); IsFailed(init_result))
        [[unlikely]]
    {
        return init_result;
    }

    auto&         stream = inflate_stream.Get();
    std::size_t   input_offset = 0;
    std::size_t   output_base = 0;
    std::size_t   output_chunk_size = 0;
    bool          writing_overflow_probe = false;
    unsigned char overflow_probe = 0;

    const auto refresh_input = [&]()
    {
        if (stream.avail_in != 0 || input_offset >= compressed_size)
        {
            return;
        }
        const auto chunk_size =
            (std::min)(compressed_size - input_offset,
                       static_cast<std::size_t>(
                           std::numeric_limits<uInt>::max()));
        stream.next_in = reinterpret_cast<Bytef*>(
            const_cast<char*>(p_compressed_data + input_offset));
        stream.avail_in = static_cast<uInt>(chunk_size);
        input_offset += chunk_size;
    };

    const auto bytes_written = [&]() -> std::size_t
    {
        if (writing_overflow_probe)
        {
            return output_size + (1U - stream.avail_out);
        }
        return output_base + output_chunk_size - stream.avail_out;
    };

    const auto refresh_output = [&]() -> bool
    {
        if (stream.avail_out != 0)
        {
            return true;
        }

        if (!writing_overflow_probe)
        {
            output_base += output_chunk_size;
            output_chunk_size = 0;
        }

        if (output_base < output_size)
        {
            const auto chunk_size =
                (std::min)(output_size - output_base,
                           static_cast<std::size_t>(
                               std::numeric_limits<uInt>::max()));
            stream.next_out = p_output_data + output_base;
            stream.avail_out = static_cast<uInt>(chunk_size);
            output_chunk_size = chunk_size;
            return true;
        }

        if (mode == InflateMode::StopWhenOutputFull)
        {
            *p_out_bytes_written = output_size;
            return false;
        }

        writing_overflow_probe = true;
        stream.next_out = &overflow_probe;
        stream.avail_out = 1;
        output_chunk_size = 1;
        return true;
    };

    for (;;)
    {
        refresh_input();
        if (!refresh_output())
        {
            return DAS_S_OK;
        }

        const auto before_avail_in = stream.avail_in;
        const auto before_avail_out = stream.avail_out;
        const auto inflate_result = inflate(&stream, Z_NO_FLUSH);
        const auto written = bytes_written();
        *p_out_bytes_written = written > output_size ? output_size : written;

        if (inflate_result == Z_STREAM_END)
        {
            if (mode == InflateMode::RequireExactOutputSize
                && written != output_size) [[unlikely]]
            {
                const auto error_message = DAS::fmt::format(
                    "ADB gzip decompressed size mismatch: expected={}, actual={}.",
                    output_size,
                    written);
                DAS_LOG_ERROR(error_message.c_str());
                return CAPTURE_DATA_TOO_LESS;
            }
            if (mode == InflateMode::RequireExactOutputSize
                && (stream.avail_in != 0 || input_offset < compressed_size))
                [[unlikely]]
            {
                const auto remaining_input =
                    static_cast<std::size_t>(stream.avail_in)
                    + (compressed_size - input_offset);
                const auto error_message = DAS::fmt::format(
                    "ADB gzip stream ended with trailing input: remaining={}.",
                    remaining_input);
                DAS_LOG_ERROR(error_message.c_str());
                return CAPTURE_DATA_TOO_LESS;
            }
            return DAS_S_OK;
        }

        if (inflate_result != Z_OK) [[unlikely]]
        {
            const auto error_message = DAS::fmt::format(
                "ADB gzip inflate failed: result={}, message={}.",
                inflate_result,
                stream.msg != nullptr ? stream.msg : "");
            DAS_LOG_ERROR(error_message.c_str());
            return CAPTURE_DATA_TOO_LESS;
        }

        if (writing_overflow_probe && stream.avail_out == 0) [[unlikely]]
        {
            const auto error_message = DAS::fmt::format(
                "ADB gzip decompressed output is longer than expected: expected={}.",
                output_size);
            DAS_LOG_ERROR(error_message.c_str());
            return CAPTURE_DATA_TOO_LESS;
        }

        if (stream.avail_in == before_avail_in
            && stream.avail_out == before_avail_out) [[unlikely]]
        {
            DAS_LOG_ERROR("ADB gzip inflate made no progress.");
            return CAPTURE_DATA_TOO_LESS;
        }
    }
}

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
    boost::asio::readable_pipe       stdout_pipe_;

public:
    CommandExecutorContext(
        std::string_view    command,
        const std::uint32_t timeout)
        : ioc_{}, timeout_timer_{ioc_, std::chrono::seconds(timeout)}, sig_{},
          timeout_in_ms_{timeout * 1000}, command_{command},
          result_{DAS_E_UNDEFINED_RETURN_VALUE}, buffer_{}, stdout_pipe_{ioc_}
    {
        // Launch child process; pipe write end → child stdout, read end →
        // stdout_pipe_
        boost::process::v2::process child_process{
            ioc_,
            command_,
            {},
            boost::process::v2::process_stdio{{}, stdout_pipe_, {}}};

        // Drain stdout into memory buffer via async_read
        boost::asio::async_read(
            stdout_pipe_,
            boost::asio::dynamic_buffer(buffer_),
            [this](
                boost::system::error_code ec,
                std::size_t /*bytes_transferred*/)
            {
                // EOF is the normal completion when the child closes stdout
                if (ec && ec != boost::asio::error::eof)
                {
                    const auto error_message = DAS::fmt::format(
                        "Error reading stdout pipe for command {}: {}.",
                        command_,
                        ec.message());
                    DAS_LOG_ERROR(error_message.c_str());
                }
            });

        // Wait for the child process to exit
        boost::process::v2::async_execute(
            std::move(child_process),
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
                            ToString(ec.message()));
                        DAS_LOG_ERROR(error_message.c_str());
                        if (result_ != DAS_E_TIMEOUT)
                        {
                            result_ = DAS_E_INTERNAL_FATAL_ERROR;
                        }
                        timeout_timer_.cancel();
                        return;
                    }
                    else [[likely]]
                    {
                        DAS_LOG_INFO(info.c_str());
                        if (result_ != DAS_E_TIMEOUT)
                        {
                            result_ = DAS_S_OK;
                        }
                    }

                    timeout_timer_.cancel();
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

    ~CommandExecutorContext() = default;
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
    if (header.width == 0 || header.height == 0)
    {
        const auto error_message = DAS::fmt::format(
            "Invalid framebuffer dimensions: {}x{}",
            header.width,
            header.height);
        DAS_LOG_ERROR(error_message.c_str());
        return tl::make_unexpected(CAPTURE_DATA_TOO_LESS);
    }

    uint64_t bytes_per_pixel = 0;
    switch (static_cast<AdbCaptureFormat>(header.format))
    {
    case AdbCaptureFormat::RGBA_8888:
    case AdbCaptureFormat::RGBX_8888:
        bytes_per_pixel = 4;
        break;
    case AdbCaptureFormat::RGB_888:
        bytes_per_pixel = 3;
        break;
    // RGB_565 and so on.
    default:
        const auto error_message =
            DAS::fmt::format("Unsupported color format: {}", header.format);
        DAS_LOG_ERROR(error_message.c_str());
        return tl::make_unexpected(UNSUPPORTED_COLOR_FORMAT);
    }

    uint64_t pixels = 0;
    if (!TryMultiplyUint64(header.width, header.height, &pixels))
    {
        const auto error_message = DAS::fmt::format(
            "Framebuffer pixel count overflow: {}x{}",
            header.width,
            header.height);
        DAS_LOG_ERROR(error_message.c_str());
        return tl::make_unexpected(CAPTURE_DATA_TOO_LESS);
    }

    uint64_t byte_count = 0;
    if (!TryMultiplyUint64(pixels, bytes_per_pixel, &byte_count)
        || byte_count > std::numeric_limits<std::size_t>::max())
    {
        const auto error_message = DAS::fmt::format(
            "Framebuffer byte count overflow: pixels={}, bytes_per_pixel={}",
            pixels,
            bytes_per_pixel);
        DAS_LOG_ERROR(error_message.c_str());
        return tl::make_unexpected(CAPTURE_DATA_TOO_LESS);
    }

    return static_cast<std::size_t>(byte_count);
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
    return Details::CommandExecutorContext<T>{command, timeout};
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
    // Run adb and receive screen capture.
    Details::CommandExecutorContext<std::vector<char>> context{
        capture_gzip_raw_command_,
        Details::PROCESS_TIMEOUT_IN_S};

    // wait for the process to exit.
    const auto exec_result = context.Run();
    if (!IsOk(exec_result))
    {
        return exec_result;
    }
    if (context.GetBuffer().empty()) [[unlikely]]
    {
        DAS_LOG_ERROR("ADB gzip capture returned empty stdout.");
        return CAPTURE_DATA_TOO_LESS;
    }

    std::array<unsigned char, ADB_CAPTURE_HEADER_SIZE> header_buffer{};
    std::size_t                                        header_bytes = 0;
    auto inflate_result = Details::InflateGzipToBuffer(
        context.GetBuffer().data(),
        context.GetBuffer().size(),
        header_buffer.data(),
        header_buffer.size(),
        Details::InflateMode::StopWhenOutputFull,
        &header_bytes);
    if (!IsOk(inflate_result)) [[unlikely]]
    {
        return inflate_result;
    }
    if (header_bytes < ADB_CAPTURE_HEADER_SIZE) [[unlikely]]
    {
        const auto error_message = DAS::fmt::format(
            "Received truncated framebuffer header. Expected at least {} "
            "bytes, got {}.",
            ADB_CAPTURE_HEADER_SIZE,
            header_bytes);
        DAS_LOG_ERROR(error_message.c_str());
        return CAPTURE_DATA_TOO_LESS;
    }

    const auto header = Details::ResolveHeader(
        reinterpret_cast<const char*>(header_buffer.data()));
    const auto expected_payload = Details::ComputeDataSizeFromHeader(header);
    if (!expected_payload) [[unlikely]]
    {
        return expected_payload.error();
    }

    if (expected_payload.value() > std::numeric_limits<std::size_t>::max()
                                       - ADB_CAPTURE_HEADER_SIZE) [[unlikely]]
    {
        const auto error_message = DAS::fmt::format(
            "ADB framebuffer total byte count overflow: header={}, payload={}.",
            ADB_CAPTURE_HEADER_SIZE,
            expected_payload.value());
        DAS_LOG_ERROR(error_message.c_str());
        return CAPTURE_DATA_TOO_LESS;
    }

    if (header.width
            > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
        || header.height > static_cast<uint32_t>(
               std::numeric_limits<int32_t>::max())) [[unlikely]]
    {
        const auto error_message = DAS::fmt::format(
            "ADB framebuffer dimensions exceed DasSize range: {}x{}.",
            header.width,
            header.height);
        DAS_LOG_ERROR(error_message.c_str());
        return DAS_E_OUT_OF_RANGE;
    }

    const auto total_decompressed_size =
        ADB_CAPTURE_HEADER_SIZE + expected_payload.value();
    DasPtr<ExportInterface::IDasMemory> exact_memory;
    const auto                          create_memory_result =
        ::CreateIDasMemory(total_decompressed_size, exact_memory.Put());
    if (!IsOk(create_memory_result)) [[unlikely]]
    {
        return create_memory_result;
    }
    if (!exact_memory) [[unlikely]]
    {
        return DAS_E_INVALID_POINTER;
    }

    DasPtr<ExportInterface::IDasBinaryBuffer> memory_buffer;
    auto                                      get_buffer_result =
        exact_memory->GetMutableView(0, memory_buffer.Put());
    if (!IsOk(get_buffer_result)) [[unlikely]]
    {
        return get_buffer_result;
    }
    if (!memory_buffer) [[unlikely]]
    {
        return DAS_E_INVALID_POINTER;
    }

    uint64_t memory_buffer_size = 0;
    auto     get_size_result = memory_buffer->GetSize(&memory_buffer_size);
    if (!IsOk(get_size_result)) [[unlikely]]
    {
        return get_size_result;
    }
    if (memory_buffer_size < total_decompressed_size) [[unlikely]]
    {
        const auto error_message = DAS::fmt::format(
            "ADB memory view too small: expected={}, actual={}.",
            total_decompressed_size,
            memory_buffer_size);
        DAS_LOG_ERROR(error_message.c_str());
        return DAS_E_OUT_OF_RANGE;
    }

    unsigned char* p_exact_data = nullptr;
    auto           get_data_result = memory_buffer->GetData(&p_exact_data);
    if (!IsOk(get_data_result)) [[unlikely]]
    {
        return get_data_result;
    }
    if (p_exact_data == nullptr) [[unlikely]]
    {
        return DAS_E_INVALID_POINTER;
    }

    std::size_t decompressed_bytes = 0;
    inflate_result = Details::InflateGzipToBuffer(
        context.GetBuffer().data(),
        context.GetBuffer().size(),
        p_exact_data,
        total_decompressed_size,
        Details::InflateMode::RequireExactOutputSize,
        &decompressed_bytes);
    if (!IsOk(inflate_result)) [[unlikely]]
    {
        return inflate_result;
    }
    if (decompressed_bytes != total_decompressed_size) [[unlikely]]
    {
        const auto error_message = DAS::fmt::format(
            "ADB gzip decompressed size mismatch: expected={}, actual={}.",
            total_decompressed_size,
            decompressed_bytes);
        DAS_LOG_ERROR(error_message.c_str());
        return CAPTURE_DATA_TOO_LESS;
    }

    DasPtr<ExportInterface::IDasBinaryBuffer> payload_buffer;
    get_buffer_result = exact_memory->GetBinaryBuffer(
        ADB_CAPTURE_HEADER_SIZE,
        payload_buffer.Put());
    if (!IsOk(get_buffer_result)) [[unlikely]]
    {
        return get_buffer_result;
    }
    if (!payload_buffer) [[unlikely]]
    {
        return DAS_E_INVALID_POINTER;
    }

    uint64_t payload_view_size = 0;
    get_size_result = payload_buffer->GetSize(&payload_view_size);
    if (!IsOk(get_size_result)) [[unlikely]]
    {
        return get_size_result;
    }
    if (payload_view_size < expected_payload.value()) [[unlikely]]
    {
        const auto error_message = DAS::fmt::format(
            "ADB payload view too small: expected={}, actual={}.",
            expected_payload.value(),
            payload_view_size);
        DAS_LOG_ERROR(error_message.c_str());
        return CAPTURE_DATA_TOO_LESS;
    }

    unsigned char* p_payload_data = nullptr;
    get_data_result = payload_buffer->GetData(&p_payload_data);
    if (!IsOk(get_data_result)) [[unlikely]]
    {
        return get_data_result;
    }
    if (p_payload_data == nullptr) [[unlikely]]
    {
        return DAS_E_INVALID_POINTER;
    }

    ExportInterface::DasSize size{
        static_cast<int32_t>(header.width),
        static_cast<int32_t>(header.height)};
    DasPtr<ExportInterface::IDasImage> p_image{};
    switch (static_cast<AdbCaptureFormat>(header.format))
    {
    case AdbCaptureFormat::RGBA_8888:
    case AdbCaptureFormat::RGBX_8888:
    {
        try
        {
            DasPtr<ExportInterface::IDasMemory> payload_memory{
                new Details::PayloadMemoryView(payload_buffer.Get())};
            const auto create_image_result = ::CreateIDasImageFromRgb888(
                payload_memory.Get(),
                &size,
                p_image.Put());
            if (IsOk(create_image_result) && !p_image) [[unlikely]]
            {
                return DAS_E_INVALID_POINTER;
            }
            return create_image_result;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }
    case AdbCaptureFormat::RGB_888:
    {
        const auto color_format =
            Details::Convert(static_cast<AdbCaptureFormat>(header.format));
        if (!color_format) [[unlikely]]
        {
            return color_format.error();
        }
        DasImageDesc desc{
            .p_data = reinterpret_cast<char*>(p_payload_data),
            .data_size = expected_payload.value(),
            .data_format = color_format.value()};
        const auto create_image_result =
            ::CreateIDasImageFromDecodedData(&desc, &size, p_image.Put());
        if (IsOk(create_image_result) && !p_image) [[unlikely]]
        {
            return DAS_E_INVALID_POINTER;
        }
        return create_image_result;
    }
    default:
        return UNSUPPORTED_COLOR_FORMAT;
    }
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

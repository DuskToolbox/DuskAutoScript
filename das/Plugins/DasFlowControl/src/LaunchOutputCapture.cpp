#include "LaunchOutputCapture.h"

#include <das/Utils/BoostPathBridge.h>
#include <das/Utils/FileUtils.hpp>
#include <das/Utils/PathText.h>
#include <das/Utils/fmt.h>

#include <boost/dll/runtime_symbol_info.hpp>

#include <cstddef>
#include <ctime>
#include <mutex>
#include <system_error>
#include <utility>

DAS_NS_BEGIN

namespace
{
    // 捕获日志目录测试缝槽（空 = 生产默认）。互斥保护：缝注入与 Do 执行
    // 可能来自不同线程。
    std::mutex& LaunchCaptureDirectoryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::string& LaunchCaptureDirectorySlot()
    {
        static std::string directory;
        return directory;
    }

    // 文件名段清洗：进程名来自用户配置的 exe 路径，白名单外的字符替换
    // '_'（消除路径穿越与非法文件名字面），清洗后为空回退占位名。
    std::string SanitizeProcessNameForFilename(std::string_view process_name)
    {
        std::string sanitized;
        sanitized.reserve(process_name.size());
        for (const char character : process_name)
        {
            const bool allowed = (character >= 'a' && character <= 'z')
                                 || (character >= 'A' && character <= 'Z')
                                 || (character >= '0' && character <= '9')
                                 || character == '.' || character == '_'
                                 || character == '-';
            sanitized.push_back(allowed ? character : '_');
        }
        if (sanitized.empty())
        {
            sanitized = "process";
        }
        return sanitized;
    }

    // 本地时刻 yyyyMMdd-HHmmss（文件名用；时区取 spawn 时刻本地时间）。
    std::string FormatLocalTimestampForFilename()
    {
        const std::time_t now = std::time(nullptr);
        std::tm           local_time{};
#ifdef _WIN32
        localtime_s(&local_time, &now);
#else
        localtime_r(&now, &local_time);
#endif
        return DAS_FMT_NS::format(
            "{:04d}{:02d}{:02d}-{:02d}{:02d}{:02d}",
            local_time.tm_year + 1900,
            local_time.tm_mon + 1,
            local_time.tm_mday,
            local_time.tm_hour,
            local_time.tm_min,
            local_time.tm_sec);
    }
} // namespace

void SetLaunchCaptureLogDirectoryForTest(const char* utf8_directory)
{
    const std::scoped_lock lock(LaunchCaptureDirectoryMutex());
    LaunchCaptureDirectorySlot() =
        utf8_directory != nullptr ? std::string{utf8_directory} : std::string{};
}

std::string ResolveLaunchCaptureLogDirectory()
{
    {
        const std::scoped_lock lock(LaunchCaptureDirectoryMutex());
        if (!LaunchCaptureDirectorySlot().empty())
        {
            return LaunchCaptureDirectorySlot();
        }
    }
    boost::system::error_code ignored;
    const auto                plugin_directory =
        boost::dll::this_line_location(ignored).parent_path();
    return Das::Utils::PathToUtf8(
        Das::Utils::ToStdPath(plugin_directory / DAS_PATH_LITERAL("logs")));
}

LaunchOutputCaptureSession::LaunchOutputCaptureSession(
    std::filesystem::path log_directory,
    std::string           sanitized_process_name,
    std::string           timestamp_text)
    : log_directory_(std::move(log_directory)),
      process_name_(std::move(sanitized_process_name)),
      timestamp_text_(std::move(timestamp_text)), stdout_stream_(io_context_),
      stderr_stream_(io_context_)
{
}

LaunchOutputCaptureSession::~LaunchOutputCaptureSession()
{
    // 最后一个引用可能在 io 线程自身释放（线程主函数持 self，run() 已返
    // 回后才析构）——此时只能 detach（自 join 未定义）；外部线程路径
    // （spawn 失败未起线程/测试持有）run() 已无工作自然返回，join 立即
    // 完成。
    if (io_thread_.joinable())
    {
        if (io_thread_.get_id() == std::this_thread::get_id())
        {
            io_thread_.detach();
        }
        else
        {
            io_context_.stop();
            io_thread_.join();
        }
    }
}

boost::asio::readable_pipe& LaunchOutputCaptureSession::StdoutPipe()
{
    return stdout_stream_.pipe;
}

boost::asio::readable_pipe& LaunchOutputCaptureSession::StderrPipe()
{
    return stderr_stream_.pipe;
}

void LaunchOutputCaptureSession::Start(std::uint32_t process_id)
{
    const auto file_name_base = DAS_FMT_NS::format(
        "{}-{}-{}",
        timestamp_text_,
        process_name_,
        process_id);
    if (!OpenStreamFile(stdout_stream_, file_name_base + "-out.log")
        || !OpenStreamFile(stderr_stream_, file_name_base + "-err.log"))
    {
        // 降级：不投读链，会话随最后引用释放回收（管道关闭后子进程写端
        // 立即报错，不会阻塞）——启动动作不受影响。
        return;
    }
    PostStreamRead(stdout_stream_);
    PostStreamRead(stderr_stream_);
    // 线程主函数持 self：读链处理器销毁后（run() 内）会话仍存活到 run()
    // 返回，io_context 析构不与 run() 并发。
    io_thread_ =
        std::thread([self = shared_from_this()]() { self->io_context_.run(); });
}

bool LaunchOutputCaptureSession::OpenStreamFile(
    CaptureStream&   stream,
    std::string_view file_name)
{
    try
    {
        const auto file_path =
            log_directory_ / Das::Utils::PathFromUtf8(std::string{file_name});
        stream.file.open(file_path, std::ios::binary | std::ios::app);
        if (!stream.file.is_open())
        {
            const auto reason = DAS_FMT_NS::format(
                "Launch output capture degraded: cannot open log file "
                "'{}'",
                Das::Utils::PathToUtf8(file_path));
            DAS_LOG_WARNING(reason.c_str());
            return false;
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        const auto reason = DAS_FMT_NS::format(
            "Launch output capture degraded: cannot open log file: {}",
            exception.what());
        DAS_LOG_WARNING(reason.c_str());
        return false;
    }
}

void LaunchOutputCaptureSession::PostStreamRead(CaptureStream& stream)
{
    stream.pipe.async_read_some(
        boost::asio::buffer(stream.buffer),
        [self = shared_from_this(), &stream](
            const boost::system::error_code& error_code,
            std::size_t                      bytes_transferred)
        { self->HandleStreamRead(stream, error_code, bytes_transferred); });
}

void LaunchOutputCaptureSession::HandleStreamRead(
    CaptureStream&                   stream,
    const boost::system::error_code& error_code,
    std::size_t                      bytes_transferred)
{
    if (error_code)
    {
        // EOF（或管道错误）：残留无结尾换行的部分作末行冲出，本流停止续
        // 投——双流均到点后 io_context 无工作，线程自然退场。
        FlushPartialLine(stream);
        return;
    }
    try
    {
        DispatchBufferedLines(
            stream,
            std::string_view(stream.buffer.data(), bytes_transferred));
        PostStreamRead(stream);
    }
    catch (const std::exception& exception)
    {
        const auto reason = DAS_FMT_NS::format(
            "Launch output capture degraded on one stream: {}",
            exception.what());
        DAS_LOG_WARNING(reason.c_str());
    }
    catch (...)
    {
        DAS_LOG_WARNING(
            "Launch output capture degraded on one stream: unknown error");
    }
}

void LaunchOutputCaptureSession::DispatchBufferedLines(
    CaptureStream&   stream,
    std::string_view chunk)
{
    // 拆行只看 '\n'：子进程输出按不透明文本处理，不解析不执行控制序列；
    // '\r' 容忍仅剥离 \r\n 的结尾 '\r'。
    stream.accumulator.append(chunk);
    std::size_t start = 0;
    while (true)
    {
        const auto newline_position = stream.accumulator.find('\n', start);
        if (newline_position == std::string::npos)
        {
            break;
        }
        std::string_view line(
            stream.accumulator.data() + start,
            newline_position - start);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        WriteLine(stream, line);
        start = newline_position + 1;
    }
    if (start > 0)
    {
        stream.accumulator.erase(0, start);
    }
}

void LaunchOutputCaptureSession::FlushPartialLine(CaptureStream& stream)
{
    if (stream.accumulator.empty())
    {
        return;
    }
    std::string_view line(stream.accumulator);
    if (line.back() == '\r')
    {
        line.remove_suffix(1);
    }
    if (!line.empty())
    {
        WriteLine(stream, line);
    }
    stream.accumulator.clear();
}

void LaunchOutputCaptureSession::WriteLine(
    CaptureStream&   stream,
    std::string_view line)
{
    // 裸行 append：不加时间戳/流标签前缀（流归属与时刻已由文件名承载）；
    // 写后立即 flush——host 先退已写行不丢。
    stream.file.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream.file.put('\n');
    stream.file.flush();
}

std::shared_ptr<LaunchOutputCaptureSession> CreateLaunchOutputCaptureSession(
    const std::string& log_directory_utf8,
    const std::string& process_name)
{
    try
    {
        auto log_directory = Das::Utils::PathFromUtf8(log_directory_utf8);
        std::error_code error_code;
        if (!Das::Utils::CreateDirectoryRecursive(log_directory, error_code))
        {
            const auto reason = DAS_FMT_NS::format(
                "Launch output capture disabled: cannot create log directory "
                "'{}' (code = {})",
                log_directory_utf8,
                error_code.value());
            DAS_LOG_WARNING(reason.c_str());
            return nullptr;
        }
        return std::make_shared<LaunchOutputCaptureSession>(
            std::move(log_directory),
            SanitizeProcessNameForFilename(process_name),
            FormatLocalTimestampForFilename());
    }
    catch (const std::exception& exception)
    {
        const auto reason = DAS_FMT_NS::format(
            "Launch output capture disabled: {}",
            exception.what());
        DAS_LOG_WARNING(reason.c_str());
        return nullptr;
    }
}

DAS_NS_END

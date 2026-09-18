#ifndef DAS_PLUGINS_DASFLOWCONTROL_LAUNCHOUTPUTCAPTURE_H
#define DAS_PLUGINS_DASFLOWCONTROL_LAUNCHOUTPUTCAPTURE_H

#include <das/DasApi.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_PROCESS_WARNING

#include <boost/asio/io_context.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/system/error_code.hpp>

DAS_DISABLE_WARNING_END

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

DAS_NS_BEGIN

// 测试缝：把捕获日志目录指向给定 UTF-8 目录（优先于生产默认）。传
// nullptr 清除，恢复生产默认（插件目录/logs）。
void SetLaunchCaptureLogDirectoryForTest(const char* utf8_directory);

// 捕获日志目录解析：测试缝槽非空返回槽值，否则返回生产默认
// （boost::dll::this_line_location 推导插件目录 + "logs"，与 cwd 无关）。
std::string ResolveLaunchCaptureLogDirectory();

// 捕获会话：持有双管道与落盘文件，子进程 spawn+detach 后调 Start 开读。
// 会话生命周期由 async 完成处理器经 shared_from_this 自持——双管道 EOF
// 后不再续投，io_context 无工作 run() 返回、线程自然退场、最后引用释放
// 时析构关文件。全程不持有 process 句柄、不 Terminate、不 wait：host/组件
// 退出绝不连带杀被启动进程（与 agent 会话的 Terminate/wait 语义相反——
// 那是宿主主动管理子进程寿命，这里是纯旁观落盘）。
class LaunchOutputCaptureSession final
    : public std::enable_shared_from_this<LaunchOutputCaptureSession>
{
public:
    LaunchOutputCaptureSession(
        std::filesystem::path log_directory,
        std::string           sanitized_process_name,
        std::string           timestamp_text);
    ~LaunchOutputCaptureSession();

    LaunchOutputCaptureSession(const LaunchOutputCaptureSession&) = delete;
    LaunchOutputCaptureSession& operator=(const LaunchOutputCaptureSession&) =
        delete;

    // 子进程 stdio 绑定用（bp2 在 process 构造期创建管道对并把读端 assign
    // 进对象——调用前无需 open，仅需绑对 io_context）。
    boost::asio::readable_pipe& StdoutPipe();
    boost::asio::readable_pipe& StderrPipe();

    // spawn+detach 成功后恰调用一次：按 pid 定名打开两个日志文件、投递双
    // 读链、起 io 线程。文件打开失败仅告警并放弃读取（启动动作不受影响）。
    void Start(std::uint32_t process_id);

private:
    struct CaptureStream
    {
        explicit CaptureStream(boost::asio::io_context& io_context)
            : pipe(io_context)
        {
        }

        boost::asio::readable_pipe pipe;
        std::ofstream              file;
        std::string                accumulator;
        std::array<char, 4096>     buffer{};
    };

    bool OpenStreamFile(CaptureStream& stream, std::string_view file_name);
    void PostStreamRead(CaptureStream& stream);
    void HandleStreamRead(
        CaptureStream&                   stream,
        const boost::system::error_code& error_code,
        std::size_t                      bytes_transferred);
    void DispatchBufferedLines(CaptureStream& stream, std::string_view chunk);
    void FlushPartialLine(CaptureStream& stream);
    void WriteLine(CaptureStream& stream, std::string_view line);

    std::filesystem::path   log_directory_;
    std::string             process_name_;
    std::string             timestamp_text_;
    boost::asio::io_context io_context_;
    CaptureStream           stdout_stream_;
    CaptureStream           stderr_stream_;
    std::thread             io_thread_;
};

// 工厂：建目录 + 构造会话。任一步失败 DAS_LOG_WARNING 后返回 nullptr，
// 调用方降级三路 stdio nullptr 丢弃输出（绝不阻塞启动）。process_name 为
// exe 文件名去扩展名的文本，文件名白名单清洗在工厂内完成。
std::shared_ptr<LaunchOutputCaptureSession> CreateLaunchOutputCaptureSession(
    const std::string& log_directory_utf8,
    const std::string& process_name);

DAS_NS_END

#endif // DAS_PLUGINS_DASFLOWCONTROL_LAUNCHOUTPUTCAPTURE_H

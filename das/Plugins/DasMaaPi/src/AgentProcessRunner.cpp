#include "AgentProcessRunner.h"

#include <das/DasApi.h>
#include <das/Utils/BoostPathBridge.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_PROCESS_WARNING

#include <boost/asio.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#ifdef _WIN32
// creation_flags.hpp 依赖 Windows 类型（DWORD / CREATE_NEW_PROCESS_GROUP），
// 必须整块守卫——POSIX 上不存在这些类型，不守卫 include 会编译错。
#include <boost/process/v2/windows/creation_flags.hpp>
#endif

DAS_DISABLE_WARNING_END

namespace Das::Plugins::DasMaaPi::AgentRuntime
{
    namespace
    {
        bool StartsWithPi(std::string_view value)
        {
            return value.starts_with("PI_");
        }

        // 把 chunk 追加进 accumulator，按 '\n' 拆出每个完整行（去掉换行符）调
        // on_output；末尾若不含 '\n'，残留留在 accumulator 等下一次读。返回发出
        // 的行数。chunk 为空 view 时无副作用（仅用于 EOF flush 调用前的空读）。
        // on_output 异常被 try/catch 兜底（DAS_LOG_WARNING），单行异常不阻断
        // 后续行分发。tampering 缓解：agent 输出按不透明文本处理，不执行任何
        // 控制序列（拆分只看 '\n'）。
        std::size_t DispatchBufferedLines(
            std::string&     accumulator,
            std::string_view chunk,
            bool             is_stderr,
            const std::function<void(std::string_view line, bool is_stderr)>&
                on_output)
        {
            if (!on_output || chunk.empty())
            {
                accumulator.append(chunk);
                return 0;
            }
            accumulator.append(chunk);
            std::size_t emitted = 0;
            std::size_t start = 0;
            while (true)
            {
                const auto newline = accumulator.find('\n', start);
                if (newline == std::string::npos)
                {
                    break;
                }
                std::string_view line(
                    accumulator.data() + start,
                    newline - start);
                // 跨平台容忍 '\r\n'：去掉末尾 '\r'（Windows 子进程常见）。
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                try
                {
                    on_output(line, is_stderr);
                }
                catch (const std::exception& exception)
                {
                    const auto reason = std::format(
                        "AgentProcess on_output threw: {}",
                        exception.what());
                    DAS_LOG_WARNING(reason.c_str());
                }
                catch (...)
                {
                    DAS_LOG_WARNING(
                        "AgentProcess on_output threw a non-std exception");
                }
                ++emitted;
                start = newline + 1;
            }
            // 已分发的行从 accumulator 头部移除，剩 partial 留待下次。
            if (start > 0)
            {
                accumulator.erase(0, start);
            }
            return emitted;
        }

        // EOF flush：accumulator 里残留的 partial（无结尾 '\n'）作末行发出。
        // 空字符串不发（避免空末行噪声）。
        void FlushBufferedLines(
            std::string& accumulator,
            bool         is_stderr,
            const std::function<void(std::string_view line, bool is_stderr)>&
                on_output)
        {
            if (!on_output || accumulator.empty())
            {
                accumulator.clear();
                return;
            }
            std::string_view line(accumulator);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (!line.empty())
            {
                try
                {
                    on_output(line, is_stderr);
                }
                catch (const std::exception& exception)
                {
                    const auto reason = std::format(
                        "AgentProcess on_output threw on flush: {}",
                        exception.what());
                    DAS_LOG_WARNING(reason.c_str());
                }
                catch (...)
                {
                    DAS_LOG_WARNING(
                        "AgentProcess on_output threw a non-std exception on flush");
                }
            }
            accumulator.clear();
        }

        void AppendBounded(
            std::string&      target,
            const char*       data,
            std::size_t       size,
            const std::size_t max_bytes)
        {
            if (max_bytes == 0 || size == 0)
            {
                return;
            }
            target.append(data, size);
            if (target.size() > max_bytes)
            {
                target.erase(0, target.size() - max_bytes);
            }
        }
    } // namespace

    // BuildProcessEnvironment 从匿名 namespace 提升（task 5 Open Question
    // #1）： Detail 子 namespace 让单测经 test-only overload 注入 fixture 继承
    // env。 既有 BoostAgentProcess 构造内调用经 Detail::
    // 限定命中（source-compatible 提升，调用点零签名变化）。StartsWithPi
    // 仍在匿名 namespace（内部链接）， 经 Das::Plugins::DasMaaPi::AgentRuntime
    // 外层作用域 unqualified lookup 可见。
    namespace Detail
    {
        std::vector<std::string> BuildProcessEnvironment(
            const std::vector<PiEnvVarDto>& pi_env,
            const std::vector<std::pair<std::string, std::string>>&
                inherited_env)
        {
            std::map<std::string, std::string> values;
            for (const auto& [key, value] : inherited_env)
            {
                values[key] = value;
            }
            for (const auto& item : pi_env)
            {
                if (StartsWithPi(item.key))
                {
                    values[item.key] = item.value;
                }
            }
            // PYTHON* 无条件覆盖注入（operator[] 非 try_emplace）：对齐 MXU
            // maa_agent.rs:360-361 .env() 覆盖语义；注入位置在 PI_* filter
            // 之后，保证继承进程环境的同名值被覆盖、且 PI_* filter 通道不受
            // 干扰（PYTHON* 非 PI_*，无"作者优先"伪概念）。
            values["PYTHONIOENCODING"] = "utf-8";
            values["PYTHONUTF8"] = "1";

            std::vector<std::string> result;
            result.reserve(values.size());
            for (const auto& [key, value] : values)
            {
                result.push_back(key + "=" + value);
            }
            return result;
        }

        std::vector<std::string> BuildProcessEnvironment(
            const std::vector<PiEnvVarDto>& pi_env)
        {
            std::vector<std::pair<std::string, std::string>> inherited;
            // 空值条目（KEY=）直写空串，不可转换条目跳过并 WARN：
            // https://github.com/boostorg/process/issues/554
            // （size_as_utf8 缺 size==0 守卫，空串转换抛 system:87）
            for (const auto item : boost::process::v2::environment::current())
            {
                try
                {
                    if (item.value().size() == 0)
                    {
                        inherited.emplace_back(
                            item.key().string(),
                            std::string{});
                        continue;
                    }
                    inherited.emplace_back(
                        item.key().string(),
                        item.value().string());
                }
                catch (const boost::system::system_error& error)
                {
                    std::string entry_name = "[unconvertible]";
                    try
                    {
                        entry_name = item.key().string();
                    }
                    catch (const boost::system::system_error&)
                    {
                    }
                    const auto reason = std::format(
                        "Process environment entry is not convertible to "
                        "UTF-8 and will be skipped: name = {}, result = {}",
                        entry_name,
                        error.code().value());
                    DAS_LOG_WARNING(reason.c_str());
                }
            }
            return BuildProcessEnvironment(pi_env, inherited);
        }
    } // namespace Detail

    namespace
    {

        class BoostAgentProcess final : public IAgentProcess
        {
        public:
            explicit BoostAgentProcess(const AgentProcessLaunchRequest& request)
                : stdout_pipe_(io_context_), stderr_pipe_(io_context_),
                  max_output_tail_bytes_(request.max_output_tail_bytes),
                  on_output_(request.on_output)
            {
                auto env_values =
                    Detail::BuildProcessEnvironment(request.environment);
                boost::process::v2::process_environment child_environment{
                    env_values};

                if (request.capture_output)
                {
                    // bp2 的 exe 与 start_dir 形参是 boost path：窄串经
                    // ACP 构造会双损（中文路径下启动错位），全程保 path
                    // 进出；arguments 形参是窄 UTF-8 通道，保持直传。
                    process_ = std::make_unique<boost::process::v2::process>(
                        io_context_,
                        Das::Utils::ToBoostPath(request.executable),
                        request.arguments,
                        boost::process::v2::process_start_dir(
                            Das::Utils::ToBoostPath(request.working_directory)),
                        // bp2 语义：stdin 位 {}（默认构造）= 继承父进程
                        // 句柄，无控制台宿主（GUI 子系统进程）的
                        // GetStdHandle 返回 NULL → CreateProcessW 失败；
                        // nullptr = 重定向到 NUL 设备。
                        boost::process::v2::process_stdio{
                            nullptr,
                            stdout_pipe_,
                            stderr_pipe_},
                        std::move(child_environment)
#ifdef _WIN32
                            ,
                        boost::process::v2::windows::process_creation_flags<
                            CREATE_NO_WINDOW>{}
#endif
                    );
                    StartReadStdout();
                    StartReadStderr();
                }
                else
                {
                    process_ = std::make_unique<boost::process::v2::process>(
                        io_context_,
                        Das::Utils::ToBoostPath(request.executable),
                        request.arguments,
                        boost::process::v2::process_start_dir(
                            Das::Utils::ToBoostPath(request.working_directory)),
                        boost::process::v2::process_stdio{
                            nullptr,
                            nullptr,
                            nullptr},
                        std::move(child_environment)
#ifdef _WIN32
                            ,
                        boost::process::v2::windows::process_creation_flags<
                            CREATE_NO_WINDOW>{}
#endif
                    );
                }

                pid_ = static_cast<uint32_t>(process_->id());
                process_->async_wait(
                    [this](boost::system::error_code ec, int exit_code)
                    { OnExited(ec, exit_code); });
                io_thread_ = std::thread([this]() { io_context_.run(); });
            }

            BoostAgentProcess(const BoostAgentProcess&) = delete;
            BoostAgentProcess& operator=(const BoostAgentProcess&) = delete;

            ~BoostAgentProcess() override
            {
                Terminate();
                if (process_)
                {
                    boost::system::error_code ec;
                    process_->wait(ec);
                }
                stdout_pipe_.close();
                stderr_pipe_.close();
                io_context_.stop();
                if (io_thread_.joinable())
                {
                    io_thread_.join();
                }
            }

            AgentProcessSnapshot Snapshot() const override
            {
                const std::scoped_lock lock(mutex_);
                return AgentProcessSnapshot{
                    .running = running_,
                    .pid = pid_,
                    .exit_code = exit_code_,
                    .stdout_tail = stdout_tail_,
                    .stderr_tail = stderr_tail_};
            }

            bool WaitForExit(std::chrono::milliseconds timeout) override
            {
                std::unique_lock lock(mutex_);
                if (!running_)
                {
                    return true;
                }
                if (timeout.count() <= 0)
                {
                    return false;
                }
                return exited_.wait_for(
                    lock,
                    timeout,
                    [this]() { return !running_; });
            }

            void Terminate() override
            {
                boost::system::error_code ec;
                if (!process_)
                {
                    return;
                }
                if (process_->running(ec))
                {
                    process_->terminate(ec);
                }
            }

        private:
            void StartReadStdout()
            {
                stdout_pipe_.async_read_some(
                    boost::asio::buffer(stdout_buffer_),
                    [this](boost::system::error_code ec, std::size_t size)
                    {
                        if (!ec)
                        {
                            {
                                const std::scoped_lock lock(mutex_);
                                AppendBounded(
                                    stdout_tail_,
                                    stdout_buffer_.data(),
                                    size,
                                    max_output_tail_bytes_);
                            }
                            DispatchBufferedLines(
                                stdout_accumulator_,
                                std::string_view(stdout_buffer_.data(), size),
                                false,
                                on_output_);
                            StartReadStdout();
                        }
                        else
                        {
                            const std::scoped_lock lock(mutex_);
                            FlushBufferedLines(
                                stdout_accumulator_,
                                false,
                                on_output_);
                        }
                    });
            }

            void StartReadStderr()
            {
                stderr_pipe_.async_read_some(
                    boost::asio::buffer(stderr_buffer_),
                    [this](boost::system::error_code ec, std::size_t size)
                    {
                        if (!ec)
                        {
                            {
                                const std::scoped_lock lock(mutex_);
                                AppendBounded(
                                    stderr_tail_,
                                    stderr_buffer_.data(),
                                    size,
                                    max_output_tail_bytes_);
                            }
                            DispatchBufferedLines(
                                stderr_accumulator_,
                                std::string_view(stderr_buffer_.data(), size),
                                true,
                                on_output_);
                            StartReadStderr();
                        }
                        else
                        {
                            const std::scoped_lock lock(mutex_);
                            FlushBufferedLines(
                                stderr_accumulator_,
                                true,
                                on_output_);
                        }
                    });
            }

            void OnExited(boost::system::error_code ec, int exit_code)
            {
                {
                    const std::scoped_lock lock(mutex_);
                    running_ = false;
                    exit_code_ = ec ? -1 : exit_code;
                }
                exited_.notify_all();
            }

            boost::asio::io_context                      io_context_;
            boost::asio::readable_pipe                   stdout_pipe_;
            boost::asio::readable_pipe                   stderr_pipe_;
            std::unique_ptr<boost::process::v2::process> process_;
            std::thread                                  io_thread_;
            mutable std::mutex                           mutex_;
            std::condition_variable                      exited_;
            std::array<char, 4096>                       stdout_buffer_{};
            std::array<char, 4096>                       stderr_buffer_{};
            std::size_t             max_output_tail_bytes_ = 0;
            bool                    running_ = true;
            std::optional<uint32_t> pid_;
            std::optional<int32_t>  exit_code_;
            std::string             stdout_tail_;
            std::string             stderr_tail_;
            // 行分发状态：per-stream accumulator 跨 chunk 维护 partial，
            // EOF flush 末行。所有 DispatchBufferedLines/FlushBufferedLines
            // 调用 都在 mutex_ 保护下进行（与 AppendBounded 同锁），on_output
            // 在 持锁期间调出——caller 闭包不应反向调
            // BoostAgentProcess（否则死锁）。
            std::string stdout_accumulator_;
            std::string stderr_accumulator_;
            std::function<void(std::string_view line, bool is_stderr)>
                on_output_;
        };
    } // namespace

    AgentProcessLaunchResult BoostAgentProcessRunner::Launch(
        const AgentProcessLaunchRequest& request)
    {
        try
        {
            return AgentProcessLaunchResult::Success(
                std::make_unique<BoostAgentProcess>(request));
        }
        catch (const boost::system::system_error& error)
        {
            return AgentProcessLaunchResult::Failure(error.what());
        }
        catch (const std::exception& error)
        {
            return AgentProcessLaunchResult::Failure(error.what());
        }
    }
} // namespace Das::Plugins::DasMaaPi::AgentRuntime

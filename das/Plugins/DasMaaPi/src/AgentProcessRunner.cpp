#include "AgentProcessRunner.h"

#include <das/DasApi.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

namespace Das::Plugins::DasMaaPi::AgentRuntime
{
    namespace
    {
        bool StartsWithPi(std::string_view value)
        {
            return value.starts_with("PI_");
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

        std::vector<std::string> BuildProcessEnvironment(
            const std::vector<PiEnvVarDto>& pi_env)
        {
            std::map<std::string, std::string> values;
            for (const auto item : boost::process::v2::environment::current())
            {
                values[item.key().string()] = item.value().string();
            }
            for (const auto& item : pi_env)
            {
                if (StartsWithPi(item.key))
                {
                    values[item.key] = item.value;
                }
            }

            std::vector<std::string> result;
            result.reserve(values.size());
            for (const auto& [key, value] : values)
            {
                result.push_back(key + "=" + value);
            }
            return result;
        }

        class BoostAgentProcess final : public IAgentProcess
        {
        public:
            explicit BoostAgentProcess(const AgentProcessLaunchRequest& request)
                : stdout_pipe_(io_context_), stderr_pipe_(io_context_),
                  max_output_tail_bytes_(request.max_output_tail_bytes)
            {
                auto env_values = BuildProcessEnvironment(request.environment);
                boost::process::v2::process_environment child_environment{
                    env_values};

                if (request.capture_output)
                {
                    process_ = std::make_unique<boost::process::v2::process>(
                        io_context_,
                        request.executable.string(),
                        request.arguments,
                        boost::process::v2::process_start_dir(
                            request.working_directory.string()),
                        boost::process::v2::process_stdio{
                            {},
                            stdout_pipe_,
                            stderr_pipe_},
                        std::move(child_environment));
                    StartReadStdout();
                    StartReadStderr();
                }
                else
                {
                    process_ = std::make_unique<boost::process::v2::process>(
                        io_context_,
                        request.executable.string(),
                        request.arguments,
                        boost::process::v2::process_start_dir(
                            request.working_directory.string()),
                        boost::process::v2::process_stdio{{}, nullptr, nullptr},
                        std::move(child_environment));
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
                std::lock_guard lock(mutex_);
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
                                std::lock_guard lock(mutex_);
                                AppendBounded(
                                    stdout_tail_,
                                    stdout_buffer_.data(),
                                    size,
                                    max_output_tail_bytes_);
                            }
                            StartReadStdout();
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
                                std::lock_guard lock(mutex_);
                                AppendBounded(
                                    stderr_tail_,
                                    stderr_buffer_.data(),
                                    size,
                                    max_output_tail_bytes_);
                            }
                            StartReadStderr();
                        }
                    });
            }

            void OnExited(boost::system::error_code ec, int exit_code)
            {
                {
                    std::lock_guard lock(mutex_);
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

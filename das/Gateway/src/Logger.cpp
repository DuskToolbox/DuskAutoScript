#include <array>
#include <das/Gateway/Logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

DAS_GATEWAY_NS_BEGIN

std::shared_ptr<spdlog::logger>& GetLogger()
{
    static std::shared_ptr<spdlog::logger> result{
        []
        {
            const auto std_sink =
                std::make_shared<spdlog::sinks::stdout_sink_mt>();
            const auto file_sink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    "logs/" DAS_GATEWAY_NAME ".log",
                    50 * 1024 * 1024, // 50mb
                    2);

            const auto sinks =
                std::array<spdlog::sink_ptr, 2>{std_sink, file_sink};
            auto internal_result = std::make_shared<spdlog::logger>(
                "g_das_gateway_name",
                std::begin(sinks),
                std::end(sinks));
            spdlog::register_logger(internal_result);
            spdlog::set_pattern(
                "[%Y-%m-%d %H:%M:%S.%e][%t][%l][%!()][%s:%#][%i] %v");

            spdlog::set_level(spdlog::level::trace);

            SPDLOG_LOGGER_INFO(
                internal_result,
                "The gateway logger has been initialized.");

            return internal_result;
        }()};

    return result;
}

DAS_GATEWAY_NS_END

#include <das/Gateway/Logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <array>

DAS_GATEWAY_NS_BEGIN

DAS_DEFINE_VARIABLE(g_logger) = []()
{
    const auto std_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
    const auto file_sink =
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "logs/" DAS_GATEWAY_NAME ".log",
            50 * 1024 * 1024, // 50mb
            2);

    const auto sinks = std::array<spdlog::sink_ptr, 2>{std_sink, file_sink};
    const auto result = std::make_shared<spdlog::logger>(
        "g_das_gateway_name",
        std::begin(sinks),
        std::end(sinks));
    spdlog::register_logger(result);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e][%t][%l][%!()][%s:%#][%i] %v");

    spdlog::set_level(spdlog::level::trace);

    SPDLOG_LOGGER_INFO(result, "The gateway logger has been initialized.");

    return result;
}();

DAS_GATEWAY_NS_END

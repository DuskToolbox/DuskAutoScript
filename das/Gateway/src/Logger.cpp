#include <array>
#include <das/Gateway/Logger.h>
#include <das/Utils/PathText.h>
#include <filesystem>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

DAS_GATEWAY_NS_BEGIN

std::shared_ptr<spdlog::logger>& GetLogger()
{
    static std::shared_ptr<spdlog::logger> result{
        []
        {
            const auto std_sink =
                std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            // 目标文件名来自 CMake target 标识符恒 ASCII：窄入向经 ACP
            // 在任意代码页下无损；目录段走 DAS_PATH_LITERAL，native() 与
            // spdlog filename_t 每平台同型原生搬运。
            const auto file_sink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    (std::filesystem::path{DAS_PATH_LITERAL("logs")}
                     / std::filesystem::path{DAS_GATEWAY_NAME ".log"})
                        .native(),
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

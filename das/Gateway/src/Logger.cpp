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
            // 文件名基段是构建系统注入的字面量宏，与 ".log" 的拼接只能
            // 在 path 域做 +=（宽窄字符串字面量相邻拼接非法，预处理层
            // 无法完成）；native() 与 spdlog filename_t 每平台同型原生
            // 搬运。
            std::filesystem::path log_file_name{
                DAS_PATH_LITERAL(DAS_GATEWAY_NAME)};
            log_file_name += DAS_PATH_LITERAL(".log");
            const auto file_sink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    (std::filesystem::path{DAS_PATH_LITERAL("logs")}
                     / log_file_name)
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

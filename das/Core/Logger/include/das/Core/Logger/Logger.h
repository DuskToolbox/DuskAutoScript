#ifndef DAS_CORE_LOGGER_LOGGER_H
#define DAS_CORE_LOGGER_LOGGER_H

#include <das/IDasBase.h>
#include <memory>
#include <spdlog/spdlog.h>

#define DAS_CORE_LOG_INFO(...)                                                 \
    SPDLOG_LOGGER_INFO(DAS::Core::g_logger, __VA_ARGS__)
#define DAS_CORE_LOG_TRACE(...)                                                \
    SPDLOG_LOGGER_TRACE(DAS::Core::g_logger, __VA_ARGS__)
#define DAS_CORE_LOG_DEBUG(...)                                                \
    SPDLOG_LOGGER_DEBUG(DAS::Core::g_logger, __VA_ARGS__)
#define DAS_CORE_LOG_WARN(...)                                                 \
    SPDLOG_LOGGER_WARN(DAS::Core::g_logger, __VA_ARGS__)
#define DAS_CORE_LOG_ERROR(...)                                                \
    SPDLOG_LOGGER_ERROR(DAS::Core::g_logger, __VA_ARGS__)
#define DAS_CORE_LOG_CRITICAL(...)                                             \
    SPDLOG_LOGGER_CRITICAL(DAS::Core::g_logger, x)

#define DAS_CORE_LOG_WARN_USING_EXTRA_FUNCTION_NAME(function_name, ...)        \
    DAS::Core::g_logger->log(                                                  \
        ::spdlog::source_loc{__FILE__, __LINE__, function_name},               \
        ::spdlog::level::warn,                                                 \
        __VA_ARGS__)
#define DAS_CORE_LOG_ERROR_USING_EXTRA_FUNCTION_NAME(function_name, ...)       \
    DAS::Core::g_logger->log(                                                  \
        ::spdlog::source_loc{__FILE__, __LINE__, function_name},               \
        ::spdlog::level::err,                                                  \
        __VA_ARGS__)

#define DAS_CORE_TRACE_SCOPE                                                   \
    DAS::Core::TraceScope DAS_TOKEN_PASTE(                                     \
        _asr_reserved_logger_tracer_,                                          \
        __LINE__)                                                              \
    {                                                                          \
        static_cast<const char*>(__FILE__), __LINE__,                          \
            static_cast<const char*>(__FUNCTION__)                             \
    }

#define DAS_CORE_LOG_EXCEPTION(ex) DAS_CORE_LOG_ERROR(ex.what())

#define DAS_CORE_LOG_JSON_EXCEPTION(ex, key, json)                             \
    DAS_CORE_LOG_ERROR(ex.what());                                             \
    DAS_CORE_LOG_ERROR("JSON Key: {}", key);                                   \
    DAS_CORE_LOG_ERROR("----JSON dump begin----");                             \
    DAS_CORE_LOG_ERROR(json.dump());                                           \
    DAS_CORE_LOG_ERROR("----JSON dump end----")

DAS_NS_BEGIN

namespace Core
{
    extern const std::shared_ptr<spdlog::logger> g_logger;
    extern const char* const                     g_logger_name;

    class TraceScope
    {
        const char* const file_;
        int               line_;
        const char* const func_;

    public:
        TraceScope(const char* const file, int line, const char* const func);
        ~TraceScope();
    };

    template <class T>
    void LogException(const T& ex)
    {
        DAS_CORE_LOG_ERROR(ex.what());
    }

    void LogException(const auto& ex, const auto& json, const auto& key)
    {
        LogException(ex);
        DAS_CORE_LOG_ERROR("JSON Key: {}", key);
        DAS_CORE_LOG_ERROR("----JSON dump begin----");
        DAS_CORE_LOG_ERROR(json.dump());
        DAS_CORE_LOG_ERROR("----JSON dump end----");
    }
}

DAS_NS_END

#endif // DAS_CORE_LOGGER_LOGGER_H

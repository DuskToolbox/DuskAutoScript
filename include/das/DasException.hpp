#ifndef DAS_DASEXCEPTION_HPP
#define DAS_DASEXCEPTION_HPP

#include <das/DasExport.h>
#include <das/IDasBase.h>
#include <exception>
#include <memory>
#include <stdexcept>
#include <variant>

// Forward declaration
struct IDasTypeInfo;

// Win32 风格 opaque handle
typedef struct DasExceptionStringHandle_* DasExceptionStringHandle;

class das_borrow_t
{
};

struct DasExceptionSourceInfo
{
    const char* file;
    int         line;
    const char* function;
};

#define DAS_THROW_IF_FAILED_EC(...)                                            \
    {                                                                          \
        if (const auto result = __VA_ARGS__; ::Das::IsFailed(result))          \
        {                                                                      \
            DAS_THROW_EC(result);                                              \
        }                                                                      \
    }

#define DAS_THROW_EC(error_code)                                               \
    {                                                                          \
        ::DasExceptionSourceInfo __das_internal_source_location{               \
            __FILE__,                                                          \
            __LINE__,                                                          \
            DAS_FUNCTION};                                                     \
        ::DasExceptionStringHandle* p_handle = nullptr;                        \
        ::CreateDasExceptionString(                                            \
            error_code,                                                        \
            &__das_internal_source_location,                                   \
            &p_handle);                                                        \
        throw DasException{error_code, p_handle};                              \
    }

#define DAS_THROW_MSG(error_code, error_message)                               \
    {                                                                          \
        ::DasExceptionSourceInfo __das_internal_source_location{               \
            __FILE__,                                                          \
            __LINE__,                                                          \
            DAS_FUNCTION};                                                     \
        ::DasExceptionStringHandle* p_handle = nullptr;                        \
        ::CreateDasExceptionString(                                            \
            error_code,                                                        \
            error_message,                                                     \
            &__das_internal_source_location,                                   \
            &p_handle);                                                        \
        throw DasException{error_code, p_handle};                              \
    }

// C API 函数声明（实现在 DasExceptionSupport.cpp）
DAS_C_API void CreateDasExceptionString(
    DasResult                  error_code,
    DasExceptionSourceInfo*    p_source_info,
    DasExceptionStringHandle** pp_out_handle);

DAS_C_API void DeleteDasExceptionString(DasExceptionStringHandle* p_handle);

DAS_C_API const char* GetDasExceptionStringCStr(
    DasExceptionStringHandle* p_handle);

class DasException : public std::runtime_error
{
    template <class... Ts>
    struct overload_set : Ts...
    {
        using Ts::operator()...;
    };

    DasResult error_code_;
    std::variant<std::string, std::shared_ptr<DasExceptionStringHandle>>
        common_string_;

    using Base = std::runtime_error;

public:
    DasException(DasResult error_code, std::string&& string)
        : Base{string.c_str()}, error_code_{error_code},
          common_string_{std::move(string)}
    {
    }

    DasException(DasResult error_code, const char* p_string, das_borrow_t)
        : Base{p_string}, error_code_{error_code}, common_string_{p_string}
    {
    }

    DasException(DasResult error_code, const std::string& message)
        : DasException{error_code, std::string{message}}
    {
    }

    DasException(DasResult error_code, DasExceptionStringHandle* p_handle)
        : Base{GetDasExceptionStringCStr(p_handle)}, error_code_{error_code},
          common_string_{std::shared_ptr<DasExceptionStringHandle>{
              p_handle,
              DeleteDasExceptionString}}
    {
    }
    [[nodiscard]]
    const char* what() const noexcept override
    {
        return std::visit(
            overload_set{
                [](const std::string& result) { return result.c_str(); },
                [](const std::shared_ptr<DasExceptionStringHandle>& result)
                {
                    // 安全访问，无需 std::launder
                    return GetDasExceptionStringCStr(result.get());
                }},
            common_string_);
    }

    [[nodiscard]]
    auto GetErrorCode() const noexcept -> DasResult
    {
        return error_code_;
    }
};

// ----------------------------------- IMPL ------------------------------------

#endif // DAS_DASEXCEPTION_HPP

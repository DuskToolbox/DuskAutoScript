#ifndef DAS_DASEXCEPTION_HPP
#define DAS_DASEXCEPTION_HPP

#include <das/IDasBase.h>
#include <exception>
#include <memory>
#include <stdexcept>
#include <variant>

// Forward declaration
struct IDasTypeInfo;

typedef struct DasExceptionStringHandle DasExceptionStringHandle;

// Custom deleter for DasExceptionStringHandle
// Converts handle to std::string* using std::launder, then deletes via
// DeleteDasExceptionString
struct DasExceptionStringHandleDeleter
{
    void operator()(DasExceptionStringHandle* p) const noexcept
    {
        if (p)
        {
            // CRITICAL: Convert handle to std::string* BEFORE delete
            // The handle is a laundered std::string*, so we must launder it
            // back to std::string*
            std::string* p_string =
                std::launder(reinterpret_cast<std::string*>(p));
            delete p_string;
        }
    }
};

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

DAS_C_API void CreateDasExceptionString(
    DasResult                  error_code,
    DasExceptionSourceInfo*    p_source_info,
    DasExceptionStringHandle** pp_out_handle);

DAS_C_API void DeleteDasExceptionString(DasExceptionStringHandle* p_handle);

DAS_C_API const char* GetDasExceptionStringCStr(
    DasExceptionStringHandle* p_handle);

class DasException : public std::exception
{
    friend void CreateDasExceptionString(
        DasResult,
        DasExceptionSourceInfo*,
        DasExceptionStringHandle**);

    friend void DeleteDasExceptionString(DasExceptionStringHandle*);

    template <class... Ts>
    struct overload_set : Ts...
    {
        using Ts::operator()...;
    };

    DasResult error_code_;
    std::variant<
        std::string,
        std::unique_ptr<
            DasExceptionStringHandle,
            DasExceptionStringHandleDeleter>>
        common_string_;

    using Base = std::runtime_error;

protected:
    DasException(DasResult error_code, std::string&& string);
    DasException(DasResult error_code, const char* p_string, das_borrow_t);
    DasException(DasResult error_code, const std::string& message);

public:
    [[nodiscard]]
    const char* what() const noexcept override
    {
        try
        {
            return std::visit(
                overload_set{
                    [](const std::string& result) { return result.c_str(); },
                    [](const std::unique_ptr<
                        DasExceptionStringHandle,
                        DasExceptionStringHandleDeleter>& result)
                    {
                        // The handle points to std::string internally, use
                        // std::launder to convert
                        std::string* p_string = std::launder(
                            reinterpret_cast<std::string*>(result.get()));
                        return p_string->c_str();
                    }},
                common_string_);
        }
        catch (const std::exception& ex)
        {
            return ex.what();
        }
    }

    auto GetErrorCode() const noexcept -> DasResult { return error_code_; }
};

// ----------------------------------- IMPL ------------------------------------

#endif // DAS_DASEXCEPTION_HPP

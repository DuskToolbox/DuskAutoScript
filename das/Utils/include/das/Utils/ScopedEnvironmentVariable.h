#ifndef DAS_UTILS_SCOPED_ENVIRONMENT_VARIABLE_H
#define DAS_UTILS_SCOPED_ENVIRONMENT_VARIABLE_H

#include <das/Utils/Config.h>
#include <das/Utils/PathText.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <stdlib.h>
#endif

DAS_UTILS_NS_BEGIN

/// RAII 环境变量写作用域：构造快照原值，析构原样归还。值的读写必须与
/// PathFromEnvironment 的 _wdupenv_s 读端同走宽通道——窄 _putenv_s 经
/// ANSI 代码页写入，非 ASCII 路径值会被损坏且与宽读端断裂；文本面统一
/// 仓内 UTF-8 约定。
class ScopedEnvironmentVariable
{
public:
    /// 只快照不动环境：写入交给 Set/Clear，析构按快照归还。
    explicit ScopedEnvironmentVariable(const char* name) : name_{name}
    {
        original_ = ReadCurrent();
    }

    /// 快照后立即写入 initial_value（nullopt 表示清除）。
    ScopedEnvironmentVariable(
        const char*                name,
        std::optional<std::string> initial_value)
        : ScopedEnvironmentVariable{name}
    {
        Write(std::move(initial_value));
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) =
        delete;

    ~ScopedEnvironmentVariable() { Write(std::move(original_)); }

    void Set(std::string_view value) { Write(std::string{value}); }

    void Clear() { Write(std::nullopt); }

private:
    std::optional<std::string> ReadCurrent() const
    {
#ifdef _WIN32
        wchar_t* current = nullptr;
        size_t   current_length = 0;
        if (_wdupenv_s(&current, &current_length, WideName().c_str()) != 0
            || current == nullptr)
        {
            return std::nullopt;
        }
        const std::string current_text = PathToUtf8(
            std::filesystem::path{
                std::wstring_view{current, current_length - 1}});
        free(current);
        return current_text;
#else
        const char* current = std::getenv(name_.c_str());
        if (current == nullptr)
        {
            return std::nullopt;
        }
        return std::string{current};
#endif
    }

    void Write(std::optional<std::string> value)
    {
#ifdef _WIN32
        _wputenv_s(
            WideName().c_str(),
            value ? PathFromUtf8(*value).native().c_str() : L"");
#else
        if (value)
        {
            setenv(name_.c_str(), value->c_str(), 1);
        }
        else
        {
            unsetenv(name_.c_str());
        }
#endif
    }

#ifdef _WIN32
    std::wstring WideName() const
    {
        return std::wstring{name_.begin(), name_.end()};
    }
#endif

    std::string                name_;
    std::optional<std::string> original_;
};

DAS_UTILS_NS_END

#endif // DAS_UTILS_SCOPED_ENVIRONMENT_VARIABLE_H

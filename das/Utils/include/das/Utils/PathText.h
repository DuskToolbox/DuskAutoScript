#ifndef DAS_UTILS_PATHTEXT_H
#define DAS_UTILS_PATHTEXT_H

#include <das/Utils/Config.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

/// path 字面量的平台宽度分流：给字面量加 L 前缀只能由预处理完成，
/// 函数与模板都接触不到字面量的词法形态，这是 helper 家族唯一的宏
/// 成员。双层展开让参数先求值再拼接，宏字面量（如构建系统注入的
/// DLL 名）也能作入参。
#ifdef DAS_WINDOWS
#define DAS_PATH_LITERAL_IMPL(text) L##text
#define DAS_PATH_LITERAL(text) DAS_PATH_LITERAL_IMPL(text)
#else
#define DAS_PATH_LITERAL(text) text
#endif

DAS_UTILS_NS_BEGIN

/// UTF-8 文本物化为 path 的唯一入向通道。标准保证 char8_t 序列按
/// UTF-8 解码（u8path 自由函数已弃用）；Windows 窄构造走 ANSI 代码页，
/// 非 ASCII 名会损坏——任何隐式 ACP 介入都会破坏中文往返。
inline std::filesystem::path PathFromUtf8(std::string_view utf8_text)
{
    return std::filesystem::path{std::u8string_view{
        reinterpret_cast<const char8_t*>(utf8_text.data()),
        utf8_text.size()}};
}

/// path 出向为 UTF-8 文本的唯一通道：u8string() 后 char8_t→char 1:1
/// 拷贝（native 分隔符；需要 '/' 分隔符的消费点位改用
/// PathToGenericUtf8）。Windows 的 .string() 走 ANSI 代码页，非 ASCII
/// 名会损坏。
inline std::string PathToUtf8(const std::filesystem::path& source_path)
{
    const std::u8string u8_text = source_path.u8string();
    return std::string{
        reinterpret_cast<const char*>(u8_text.data()),
        u8_text.size()};
}

/// path 出向为正斜杠 + UTF-8 文本（JSON 文本域等按 '/' 约定消费的点位）：
/// generic_string() 走 ANSI 代码页，是它的非法通道。
inline std::string PathToGenericUtf8(const std::filesystem::path& source_path)
{
    const std::u8string u8_text = source_path.generic_u8string();
    return std::string{
        reinterpret_cast<const char*>(u8_text.data()),
        u8_text.size()};
}

/// 环境变量值的原生宽度获取：宽 value_type 平台走 _wdupenv_s 快照直构、
/// 窄 value_type 平台走 getenv 本码直构——消灭转换而非修转换（Windows
/// 的窄 getenv 值经 ANSI 代码页，非 ASCII 路径会损坏）。环境变量名是
/// ASCII 标识符，展宽无损。
inline std::optional<std::filesystem::path> PathFromEnvironment(
    const char* name)
{
    if constexpr (
        std::is_same_v<typename std::filesystem::path::value_type, wchar_t>)
    {
        std::wstring wide_name;
        for (const char* cursor = name; *cursor != '\0'; ++cursor)
        {
            wide_name.push_back(
                static_cast<wchar_t>(static_cast<unsigned char>(*cursor)));
        }
        wchar_t*      wide_value = nullptr;
        size_t        wide_value_length = 0;
        const errno_t duplicate_result =
            _wdupenv_s(&wide_value, &wide_value_length, wide_name.c_str());
        if (duplicate_result != 0 || wide_value == nullptr)
        {
            return std::nullopt;
        }
        const std::filesystem::path environment_path{
            std::wstring_view{wide_value, wide_value_length - 1}};
        free(wide_value);
        return environment_path;
    }
    else
    {
        const char* narrow_value = std::getenv(name);
        if (narrow_value == nullptr)
        {
            return std::nullopt;
        }
        return std::filesystem::path{std::string_view{narrow_value}};
    }
}

/// 环境变量 ASCII 文本读取：MSVC 的 CRT 环境块会被 _putenv 重建，getenv
/// 返回的指针可能悬垂（C4996），故走 _dupenv_s 快照；其余平台 getenv 即
/// 标准 API。值面向 ASCII 配置文本——非 ASCII 路径值走 PathFromEnvironment
/// 的宽通道。
inline std::optional<std::string> GetEnvironmentText(const char* name)
{
#ifdef _MSC_VER
    char*  value = nullptr;
    size_t value_length = 0;
    if (_dupenv_s(&value, &value_length, name) != 0 || value == nullptr)
    {
        return std::nullopt;
    }
    const std::string environment_text{value, value_length - 1};
    free(value);
    return environment_text;
#else
    const char* value = std::getenv(name);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    return std::string{value};
#endif
}

/// 命令行参数文本物化为 path 的唯一通道。Windows CRT 交付的窄 argv 是
/// ACP 编码（区别于仓内 UTF-8 std::string 约定）——此处的窄构造是与源
/// 配对的正确通道，不是要消灭的隐式 ACP 介入；POSIX 窄 argv 跟随
/// locale，现代系统的本码即 UTF-8。
inline std::filesystem::path PathFromCommandLineArg(std::string_view arg_text)
{
    if constexpr (
        std::is_same_v<typename std::filesystem::path::value_type, wchar_t>)
    {
        return std::filesystem::path{std::string(arg_text)};
    }
    else
    {
        return PathFromUtf8(arg_text);
    }
}

/// ACP 文本转 UTF-8 的显式通道。输入是已知的 CP_ACP 字节（如 Windows
/// FormatMessageA 产出的本地化错误消息、异常 what() 携带的中文段）——
/// 此处的窄构造是与 ACP 源配对的正确转换，区别于顶部声明要消灭的隐式
/// ACP 介入；借 path 窄构造按 ACP 解入、u8string() 按 UTF-8 吐出完成转
/// 码，ASCII 段无损。POSIX 窄文本本就是 UTF-8 的 locale 无关约定，原样
/// 透传。
inline std::string AcpTextToUtf8(std::string_view acp_text)
{
#ifdef DAS_WINDOWS
    return PathToUtf8(std::filesystem::path{std::string{acp_text}});
#else
    return std::string{acp_text};
#endif
}

DAS_UTILS_NS_END

#endif // DAS_UTILS_PATHTEXT_H

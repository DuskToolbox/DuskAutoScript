#ifndef DAS_UTILS_BOOSTPATHBRIDGE_H
#define DAS_UTILS_BOOSTPATHBRIDGE_H

#include <das/Utils/Config.h>

#include <boost/filesystem.hpp>

#include <filesystem>
#include <type_traits>

DAS_UTILS_NS_BEGIN

/// boost::filesystem::path 与 std::filesystem::path 的互转桥。boost path
/// 没有 u8string()（那是 std path 的方法），Windows 上 .string() 是 ACP
/// narrow（中文路径会乱码）。两族 path 在每平台跟随原生宽度、value_type
/// 恒同型：native() 原生串容器搬运即零编码转换。std path 是路径文本的
/// 全仓规范载体，boost path 仅在 boost API 边界经本桥进出。
template <typename TargetPath, typename SourcePath>
TargetPath PathAs(const SourcePath& source_path)
{
    if constexpr (
        std::is_same_v<
            typename SourcePath::value_type,
            typename TargetPath::value_type>)
    {
        return TargetPath{source_path.native()};
    }
    else
    {
        return TargetPath{source_path.wstring()};
    }
}

inline boost::filesystem::path ToBoostPath(
    const std::filesystem::path& std_path)
{
    return PathAs<boost::filesystem::path>(std_path);
}

inline std::filesystem::path ToStdPath(
    const boost::filesystem::path& boost_path)
{
    return PathAs<std::filesystem::path>(boost_path);
}

DAS_UTILS_NS_END

#endif // DAS_UTILS_BOOSTPATHBRIDGE_H

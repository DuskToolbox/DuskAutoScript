#include "PathResolver.h"

#include <das/Utils/PathText.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        // Windows 下 PI 的 exec 常为跨平台无扩展名写法（如 agent/go-service，
        // 实际文件是 go-service.exe）。Windows 进程创建对命令行首参会自动
        // 尝试 .exe，但对显式指定的可执行路径不做扩展名补全——boost::
        // process v2 走后者，无扩展名路径直接 spawn 失败。这里在解析结果为
        // 绝对路径、不存在、无扩展名、且同位置 <name>.exe 存在时补 .exe。
        // 显式存在的无扩展名文件（脚本等）与真正缺失的路径（保留原样让
        // spawn 报真实错误）都不受影响。
        void AppendWindowsExecutableExtensionIfNeeded(
            std::filesystem::path& resolved_path)
        {
            if (!resolved_path.is_absolute() || resolved_path.has_extension())
            {
                return;
            }
            std::error_code error_code;
            if (std::filesystem::exists(resolved_path, error_code))
            {
                return;
            }
            auto executable_path = resolved_path;
            executable_path += DAS_PATH_LITERAL(".exe");
            if (std::filesystem::exists(executable_path, error_code))
            {
                resolved_path = std::move(executable_path);
            }
        }
    } // namespace
} // namespace Das::Plugins::DasMaaPi
#endif

namespace Das::Plugins::DasMaaPi
{
    bool IsBareCommand(const std::string& value)
    {
        if (value.find('/') != std::string::npos
            || value.find('\\') != std::string::npos)
        {
            return false;
        }

        const std::filesystem::path path = Das::Utils::PathFromUtf8(value);
        return !path.empty() && !path.has_parent_path() && !path.has_root_name()
               && !path.has_root_directory();
    }

    std::filesystem::path ResolveChildExec(
        const std::string& child_exec,
        const std::string& interface_directory)
    {
        const std::filesystem::path exec_path =
            Das::Utils::PathFromUtf8(child_exec);
        if (IsBareCommand(child_exec))
        {
            return exec_path;
        }
        if (exec_path.is_absolute() || exec_path.has_root_name()
            || exec_path.has_root_directory())
        {
            auto resolved_path = exec_path.lexically_normal();
#ifdef _WIN32
            AppendWindowsExecutableExtensionIfNeeded(resolved_path);
#endif
            return resolved_path;
        }
        auto joined_path =
            (Das::Utils::PathFromUtf8(interface_directory) / exec_path)
                .lexically_normal();
#ifdef _WIN32
        AppendWindowsExecutableExtensionIfNeeded(joined_path);
#endif
        return joined_path;
    }
} // namespace Das::Plugins::DasMaaPi

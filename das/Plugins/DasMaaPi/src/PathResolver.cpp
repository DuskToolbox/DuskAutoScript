#include "PathResolver.h"

namespace Das::Plugins::DasMaaPi
{
    bool IsBareCommand(const std::string& value)
    {
        if (value.find('/') != std::string::npos
            || value.find('\\') != std::string::npos)
        {
            return false;
        }

        const std::filesystem::path path(value);
        return !path.empty() && !path.has_parent_path() && !path.has_root_name()
               && !path.has_root_directory();
    }

    std::filesystem::path ResolveChildExec(
        const std::string& child_exec,
        const std::string& interface_directory)
    {
        const std::filesystem::path exec_path(child_exec);
        if (IsBareCommand(child_exec))
        {
            return exec_path;
        }
        if (exec_path.is_absolute() || exec_path.has_root_name()
            || exec_path.has_root_directory())
        {
            return exec_path.lexically_normal();
        }
        return (std::filesystem::path(interface_directory) / exec_path)
            .lexically_normal();
    }
} // namespace Das::Plugins::DasMaaPi

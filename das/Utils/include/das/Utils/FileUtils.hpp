#ifndef DAS_UTILS_FILEUTILS_HPP
#define DAS_UTILS_FILEUTILS_HPP

#include <das/Utils/Config.h>

#include <filesystem>

DAS_UTILS_NS_BEGIN

// Returns:
//   true upon success.
//   false upon failure, and set the std::error_code & err accordingly.
inline bool CreateDirectoryRecursive(
    const std::filesystem::path dirName,
    std::error_code&            err)
{
    err.clear();
    if (std::filesystem::exists(dirName))
    {
        return true;
    }
    if (!std::filesystem::create_directories(dirName, err))
    {
        if (std::filesystem::exists(dirName))
        {
            // The folder already exists:
            err.clear();
            return true;
        }
        return false;
    }
    return true;
}

DAS_UTILS_NS_END

#endif // DAS_UTILS_FILEUTILS_HPP

/**
 * @file IpcTestConfig.h
 * @brief IPC 测试共享配置
 *
 * 包含用于 IPC 测试的配置和辅助函数。
 * 被 Basic 和 Integration 测试共用。
 */

#pragma once

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <filesystem>
#include <string>

// ============================================================
// 全局测试配置 - 环境变量与超时设置
// ============================================================
//
// 环境变量：
//   DAS_DEBUG=1          - 调试模式，所有超时变为 0（无限等待）
//   DAS_HOST_EXE_PATH    - DasHost.exe 的完整路径
//   DAS_PLUGIN_DIR       - 插件目录路径
//
// 示例（Windows）：
//   set DAS_DEBUG=1 && set DAS_HOST_EXE_PATH=C:\path\DasHost.exe && set
//   DAS_PLUGIN_DIR=C:\path\plugins && IpcMultiProcessTest.exe
//
// 示例（Linux/macOS）：
//   DAS_DEBUG=1 DAS_HOST_EXE_PATH=/path/DasHost DAS_PLUGIN_DIR=/path/plugins
//   ./IpcMultiProcessTest
//

namespace IpcTestConfig
{
    namespace detail
    {
        inline bool IsDebugMode()
        {
            const char* debug = std::getenv("DAS_DEBUG");
            return debug != nullptr
                   && (std::string(debug) == "1"
                       || std::string(debug) == "true");
        }
    } // namespace detail

    /**
     * @brief 获取启动 Host 进程的超时时间（毫秒）
     *
     * 当设置 DAS_DEBUG=1 环境变量时，返回 0（无限等待）。
     * 否则返回默认的 10000 毫秒（10秒）。
     */
    inline uint32_t GetHostStartTimeoutMs()
    {
        return detail::IsDebugMode() ? 0 : 10000;
    }

    /**
     * @brief 获取加载插件的超时时间
     *
     * 当设置 DAS_DEBUG=1 环境变量时，返回 0（无限等待）。
     * 否则返回默认的 30000 毫秒（30秒）。
     */
    inline std::chrono::milliseconds GetPluginLoadTimeout()
    {
        return std::chrono::milliseconds(detail::IsDebugMode() ? 0 : 30000);
    }

    /**
     * @brief 检查是否为调试模式
     */
    inline bool IsDebugMode() { return detail::IsDebugMode(); }

    /**
     * @brief 获取 DasHost 可执行文件路径
     *
     * 从环境变量 DAS_HOST_EXE_PATH 读取。
     * @return DasHost.exe 的完整路径
     * @throws std::runtime_error 如果环境变量未设置或文件不存在
     */
    inline std::string GetDasHostPath()
    {
        const char* path = std::getenv("DAS_HOST_EXE_PATH");
        if (path == nullptr || strlen(path) == 0)
        {
            throw std::runtime_error(
                "DAS_HOST_EXE_PATH environment variable is not set");
        }
        if (!std::filesystem::exists(path))
        {
            throw std::runtime_error(
                std::string("DasHost.exe not found at: ") + path);
        }
        return path;
    }

    /**
     * @brief 获取插件目录路径
     *
     * 从环境变量 DAS_PLUGIN_DIR 读取。
     * @return 插件目录路径
     * @throws std::runtime_error 如果环境变量未设置
     */
    inline std::string GetPluginDir()
    {
        const char* plugin_dir = std::getenv("DAS_PLUGIN_DIR");
        if (plugin_dir == nullptr || strlen(plugin_dir) == 0)
        {
            throw std::runtime_error(
                "DAS_PLUGIN_DIR environment variable is not set");
        }
        return plugin_dir;
    }

    /**
     * @brief 获取测试插件 JSON 清单路径
     * @param plugin_name 插件名称（如 "IpcTestPlugin1"）
     * @return JSON 文件路径
     * @throws std::runtime_error 如果环境变量未设置或文件不存在
     */
    inline std::string GetTestPluginJsonPath(const std::string& plugin_name)
    {
        std::filesystem::path json_path =
            std::filesystem::path{GetPluginDir()}
            / DAS_FMT_NS::format("{}.json", plugin_name);

        if (!std::filesystem::exists(json_path))
        {
            throw std::runtime_error(
                "Plugin JSON not found at: " + json_path.string());
        }
        return json_path.string();
    }

} // namespace IpcTestConfig

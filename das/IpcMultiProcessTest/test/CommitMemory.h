/**
 * @file CommitMemory.h
 * @brief 跨平台提交内存余量查询（测试准入基建）
 *
 * 多个 Test.exe 并发抢占同一 commit 池，只有系统级余量才是正确的
 * 放行信号：Windows 主口径取 GetPerformanceInfo 的
 * CommitLimit−CommitTotal（页×PageSize，精确）；
 * GlobalMemoryStatusEx.ullAvailPageFile 是进程配额侧口径（≤系统级），
 * 仅在显著小于系统级值时打一行交叉告警。Linux 仅 overcommit mode 2
 * 下 CommitLimit−Committed_AS 有强制力，其余模式回落 MemAvailable
 * （尽力）；macOS 走 os_proc_available_memory 尽力降级。
 * API/解析失败返回 {0,false} 不抛异常——调用方（准入闸门）对读数
 * 失败走具名失败，绝不静默放行。
 */

#ifndef DAS_TEST_COMMIT_MEMORY_H
#define DAS_TEST_COMMIT_MEMORY_H

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef DAS_WINDOWS
// IpcMPT 测试目标外的消费树（经显式 include 路径引用本头）没有目标级
// WIN32_LEAN_AND_MEAN，先置位再引 windows.h，防 winsock v1 与消费方后续
// asio/winsock2 头冲突
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>
#endif

#if defined(__linux__)
#include <fstream>
#endif

#if defined(__APPLE__)
#include <os/proc.h>
#endif

namespace CommitMemory
{
    /**
     * @brief 系统级提交内存余量读数
     *
     * available_bytes==0 表示读数失败（调用方不得据此放行）；
     * precise=false 表示口径为尽力估计（物理余量而非 commit 余量）。
     */
    struct CommitHeadroom
    {
        std::uint64_t available_bytes{0};
        bool          precise{false};
    };

    namespace detail
    {
#ifdef DAS_WINDOWS
        /// 进程配额侧口径显著小于系统级口径时提示一行（仅一次）：差距大
        /// 说明当前进程的提交配额（作业对象/用户限制）先于机器 commit
        /// 池收紧，闸门读数仍以系统级为准，但排查排队应先查配额侧。
        inline void ReportQuotaCrossWarningOnce(
            std::uint64_t system_available_bytes)
        {
            static bool warning_emitted = false;
            if (warning_emitted)
            {
                return;
            }
            MEMORYSTATUSEX memory_status{};
            memory_status.dwLength = sizeof(memory_status);
            if (::GlobalMemoryStatusEx(&memory_status) == 0)
            {
                return;
            }
            if (static_cast<std::uint64_t>(memory_status.ullAvailPageFile) * 2
                >= system_available_bytes)
            {
                return;
            }
            warning_emitted = true;
            std::cout
                << "[das-commit-memory] cross-warn system_available_bytes="
                << system_available_bytes << " process_quota_available_bytes="
                << memory_status.ullAvailPageFile
                << " (quota side is the tighter bound)" << '\n';
        }
#endif

#if defined(__linux__)
        /// overcommit_memory==2 时 CommitLimit 才是硬上限，其余模式下
        /// Committed_AS 可超限，CommitLimit−Committed_AS 失去含义。
        inline int ReadOvercommitMemoryMode()
        {
            std::ifstream mode_file("/proc/sys/vm/overcommit_memory");
            int           mode = -1;
            mode_file >> mode;
            return mode;
        }

        /// /proc/meminfo 的数值单位是 kB
        inline bool ReadMemInfoValueBytes(
            const char*    key,
            std::uint64_t* out_bytes)
        {
            std::ifstream     meminfo_file("/proc/meminfo");
            const std::string key_prefix = std::string(key) + ":";
            std::string       line;
            while (std::getline(meminfo_file, line))
            {
                if (line.rfind(key_prefix, 0) != 0)
                {
                    continue;
                }
                *out_bytes =
                    std::strtoull(line.c_str() + key_prefix.size(), nullptr, 10)
                    * 1024;
                return true;
            }
            return false;
        }
#endif
    } // namespace detail

    /**
     * @brief 查询系统级提交内存余量
     *
     * @return CommitHeadroom 读数；失败时 available_bytes==0
     */
    inline CommitHeadroom QueryCommitHeadroom()
    {
#ifdef DAS_WINDOWS
        PERFORMANCE_INFORMATION performance_information{};
        performance_information.cb = sizeof(performance_information);
        if (::GetPerformanceInfo(
                &performance_information,
                sizeof(performance_information))
            == 0)
        {
            return {};
        }
        const std::uint64_t available_bytes =
            static_cast<std::uint64_t>(
                performance_information.CommitLimit
                - performance_information.CommitTotal)
            * performance_information.PageSize;
        detail::ReportQuotaCrossWarningOnce(available_bytes);
        return {available_bytes, true};
#elif defined(__linux__)
        if (detail::ReadOvercommitMemoryMode() == 2)
        {
            std::uint64_t commit_limit_bytes = 0;
            std::uint64_t committed_as_bytes = 0;
            if (detail::ReadMemInfoValueBytes(
                    "CommitLimit",
                    &commit_limit_bytes)
                && detail::ReadMemInfoValueBytes(
                    "Committed_AS",
                    &committed_as_bytes)
                && commit_limit_bytes >= committed_as_bytes)
            {
                return {commit_limit_bytes - committed_as_bytes, true};
            }
            return {};
        }
        std::uint64_t mem_available_bytes = 0;
        if (detail::ReadMemInfoValueBytes("MemAvailable", &mem_available_bytes))
        {
            return {mem_available_bytes, false};
        }
        return {};
#elif defined(__APPLE__)
        // os_proc_available_memory 是 per-app 尽力口径，非系统级 commit
        // 余量
        return {
            static_cast<std::uint64_t>(::os_proc_available_memory()),
            false};
#else
        // 未支持平台按读数失败处理：闸门将具名失败，不静默放行
        return {};
#endif
    }
} // namespace CommitMemory

#endif // DAS_TEST_COMMIT_MEMORY_H

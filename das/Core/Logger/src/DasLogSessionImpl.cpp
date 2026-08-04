#define _CRT_SECURE_NO_WARNINGS

#include <das/Core/Logger/DasLogSessionImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef DAS_WINDOWS
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace Das::Core
{
    namespace
    {
        uint64_t CurrentProcessId()
        {
#ifdef DAS_WINDOWS
            return static_cast<uint64_t>(::GetCurrentProcessId());
#else
            return static_cast<uint64_t>(::getpid());
#endif
        }

        // pid 是否存活；当前进程 pid 恒为存活。
        bool ProcessAlive(uint64_t pid)
        {
#ifdef DAS_WINDOWS
            if (pid == CurrentProcessId())
            {
                return true;
            }
            HANDLE handle = ::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                static_cast<DWORD>(pid));
            if (handle == nullptr)
            {
                return false; // 打不开即视为不存在/已死
            }
            DWORD      exit_code = 0;
            const bool ok = ::GetExitCodeProcess(handle, &exit_code) != FALSE;
            ::CloseHandle(handle);
            return ok && exit_code == STILL_ACTIVE;
#else
            if (pid == CurrentProcessId())
            {
                return true;
            }
            return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
        }

        // 从会话目录名解析 pid；目录名形如 `20260801-123456-8841`（三段全
        // 数字，前两段 8/6 位时间戳，末段 pid）。格式不符返回 nullopt。
        std::optional<uint64_t> ParseSessionPid(const std::string& name)
        {
            std::vector<std::string> parts;
            std::string              current;
            for (char c : name)
            {
                if (c == '-')
                {
                    if (current.empty())
                    {
                        return std::nullopt;
                    }
                    parts.push_back(std::move(current));
                    current.clear();
                }
                else
                {
                    if (c < '0' || c > '9')
                    {
                        return std::nullopt;
                    }
                    current.push_back(c);
                }
            }
            if (current.empty())
            {
                return std::nullopt;
            }
            parts.push_back(std::move(current));
            if (parts.size() != 3 || parts[0].size() != 8
                || parts[1].size() != 6)
            {
                return std::nullopt;
            }
            uint64_t pid = 0;
            for (char c : parts[2])
            {
                pid = pid * 10 + static_cast<uint64_t>(c - '0');
                if (pid > static_cast<uint64_t>(UINT32_MAX))
                {
                    return std::nullopt;
                }
            }
            return pid;
        }

        // 会话目录名 {ts}-{pid}，ts = 本地时间 %Y%m%d-%H%M%S。
        std::string SessionDirName()
        {
            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            std::tm    local{};
#ifdef DAS_WINDOWS
            localtime_s(&local, &time);
#else
            localtime_r(&time, &local);
#endif
            char buffer[32]{};
            std::strftime(buffer, sizeof buffer, "%Y%m%d-%H%M%S", &local);
            return std::string{buffer} + "-"
                   + std::to_string(CurrentProcessId());
        }

        // 扫描 debug_directory 下匹配 `{ts}-{pid}` 命名的子目录，删除 pid 不
        // 存活的孤儿会话目录。remove_all 用 error_code 版 best-effort，失败
        // 仅记日志。当前进程 pid 视为存活。
        void CleanupOrphanSessionDirs(
            const std::filesystem::path& debug_directory)
        {
            std::error_code ec;
            if (!std::filesystem::is_directory(debug_directory, ec) || ec)
            {
                return;
            }
            for (const auto& entry :
                 std::filesystem::directory_iterator(debug_directory, ec))
            {
                if (ec)
                {
                    break;
                }
                if (!entry.is_directory(ec) || ec)
                {
                    continue;
                }
                const auto name = entry.path().filename().string();
                const auto pid = ParseSessionPid(name);
                if (!pid)
                {
                    continue; // 不是 {ts}-{pid} 会话目录，跳过
                }
                if (ProcessAlive(*pid))
                {
                    continue;
                }
                std::error_code remove_ec;
                std::filesystem::remove_all(entry.path(), remove_ec);
                if (remove_ec)
                {
                    DAS_CORE_LOG_WARN(
                        "CleanupOrphanSessionDirs: failed to remove {}: {}",
                        entry.path().string(),
                        remove_ec.message());
                }
                else
                {
                    DAS_CORE_LOG_INFO(
                        "CleanupOrphanSessionDirs: removed orphan session dir {}",
                        entry.path().string());
                }
            }
        }
    } // namespace

    DasLogSessionImpl::DasLogSessionImpl(
        const std::filesystem::path& debug_directory)
    {
        // 先清理孤儿会话目录（pid 不存活的 {ts}-{pid}
        // 目录），再建立本次会话目录。
        CleanupOrphanSessionDirs(debug_directory);

        const auto resolved =
            std::filesystem::absolute(debug_directory).lexically_normal();
        session_dir_ = resolved / SessionDirName();
        user_jsonl_path_ = session_dir_ / "user.jsonl";

        std::error_code ec;
        std::filesystem::create_directories(session_dir_ / "img", ec);
        if (ec)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to create session dir: {}",
                ec.message());
            session_dir_.clear();
            user_jsonl_path_.clear();
            return;
        }
    }

    DasLogSessionImpl::~DasLogSessionImpl()
    {
        if (session_dir_.empty())
        {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(session_dir_, ec);
        if (ec)
        {
            DAS_CORE_LOG_WARN("Failed to remove session dir: {}", ec.message());
        }
    }

    DasResult DasLogSessionImpl::GetSessionDir(
        IDasReadOnlyString** pp_out_directory)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_directory);
        *pp_out_directory = nullptr;
        if (session_dir_.empty())
        {
            return DAS_E_OBJECT_NOT_INIT;
        }
        const auto directory_str = session_dir_.u8string();
        return CreateIDasReadOnlyStringFromUtf8(
            reinterpret_cast<const char*>(directory_str.c_str()),
            pp_out_directory);
    }

    DasResult DasLogSessionImpl::GetUserJsonlPath(
        IDasReadOnlyString** pp_out_path)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_path);
        *pp_out_path = nullptr;
        if (user_jsonl_path_.empty())
        {
            return DAS_E_OBJECT_NOT_INIT;
        }
        const auto path_str = user_jsonl_path_.u8string();
        return CreateIDasReadOnlyStringFromUtf8(
            reinterpret_cast<const char*>(path_str.c_str()),
            pp_out_path);
    }

    DasResult DasLogSessionImpl::NextImageSeq(uint32_t* p_out_seq)
    {
        DAS_UTILS_CHECK_POINTER(p_out_seq);
        // fetch_add 返回前一个值，+1 得到本次递增后的值（从 1 开始计数）。
        *p_out_seq = image_seq_.fetch_add(1) + 1;
        return DAS_S_OK;
    }
} // namespace Das::Core

// ------------------------------------------------------------ C ABI 工厂

DasResult CreateLogSession(
    IDasReadOnlyString*                    debug_directory,
    Das::ExportInterface::IDasLogSession** pp_out_session)
{
    DAS_UTILS_CHECK_POINTER(pp_out_session);
    DAS_UTILS_CHECK_POINTER(debug_directory);
    *pp_out_session = nullptr;
    const char* directory_utf8 = nullptr;
    const auto  dir_result = debug_directory->GetUtf8(&directory_utf8);
    if (DAS::IsFailed(dir_result))
    {
        return dir_result;
    }
    if (directory_utf8 == nullptr || directory_utf8[0] == '\0')
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    try
    {
        // utf-8 字符串经 char8_t 重解释直接构造 path（C++20 path 接受
        // char8_t*）。
        const auto directory_path = std::filesystem::path(
            reinterpret_cast<const char8_t*>(directory_utf8));
        auto* p = new Das::Core::DasLogSessionImpl{directory_path};
        *pp_out_session = p;
        p->AddRef();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

#ifndef DAS_DEBUG_CRASHDUMPWRITERWIN32_H
#define DAS_DEBUG_CRASHDUMPWRITERWIN32_H

#include <das/Debug/Config.h>

#include <filesystem>
#include <functional>

DAS_DEBUG_NS_BEGIN

namespace Detail
{
    // Win32 平台崩溃 handler 安装。dump_path_resolver 把平台异常上下文
    // （void*；Win32 实现内 cast 回 EXCEPTION_POINTERS*）映射到 dump 输出
    // 路径；nullptr 表示无 SEH 上下文（std::terminate 路径）。
    // 内部安装 SetUnhandledExceptionFilter（SEH）+ std::set_terminate（兜底），
    // 回调里 resolver 得路径 → MiniDumpWriteDump（full dump）。
    void InstallWin32CrashHandler(
        std::function<std::filesystem::path(void* exception_context)>
            dump_path_resolver);
} // namespace Detail

DAS_DEBUG_NS_END

#endif // DAS_DEBUG_CRASHDUMPWRITERWIN32_H

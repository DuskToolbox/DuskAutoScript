#include <das/Debug/CrashDumpWriterWin32.h>

// 本文件仅 Win32 编译（CMake if(WIN32) 控制），Win32 header 直接 include。

#ifdef DAS_AUTO_MINIDUMP

// windows.h 必须在 dbghelp.h 之前：dbghelp.h 内部（_dbg_LOAD_IMAGE.h）
// 依赖 windows.h 定义的 HANDLE/DWORD/PSTR 等基础类型（MinGW 下尤其严格，
// 缺则这些类型未定义，编译报 unknown type name）。顺序用 clang-format
// off 锁定，防止字母序重排。
// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <csignal>
#include <exception>
#include <filesystem>
#include <functional>

DAS_DEBUG_NS_BEGIN

namespace Detail
{
    namespace
    {
        // 进程生命周期持有的 resolver（地址固定，SEH/terminate 回调安全访问）。
        std::function<std::filesystem::path(void*)> g_dump_path_resolver;

        // 写 full minidump。exception_pointers 为空（std::terminate 路径）时
        // 不附异常记录，dump 仍含完整内存与线程栈。
        void WriteCrashDump(
            const std::filesystem::path& path,
            EXCEPTION_POINTERS*          exception_pointers)
        {
            const HANDLE file = CreateFileW(
                path.native().c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return;
            }

            MINIDUMP_EXCEPTION_INFORMATION exception_info{};
            exception_info.ThreadId = GetCurrentThreadId();
            exception_info.ExceptionPointers = exception_pointers;
            exception_info.ClientPointers = FALSE;

            MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                file,
                MiniDumpWithFullMemory,
                exception_pointers ? &exception_info : nullptr,
                nullptr,
                nullptr);

            CloseHandle(file);
        }

        LONG WINAPI
        UnhandledExceptionCallback(EXCEPTION_POINTERS* exception_pointers)
        {
            if (g_dump_path_resolver)
            {
                WriteCrashDump(
                    g_dump_path_resolver(exception_pointers),
                    exception_pointers);
            }
            return EXCEPTION_EXECUTE_HANDLER; // 写完即终止
        }

        void TerminateHandler()
        {
            if (g_dump_path_resolver)
            {
                WriteCrashDump(g_dump_path_resolver(nullptr), nullptr);
            }
            std::abort();
        }
    } // namespace

    void InstallWin32CrashHandler(
        std::function<std::filesystem::path(void*)> dump_path_resolver)
    {
        g_dump_path_resolver = std::move(dump_path_resolver);

        SetUnhandledExceptionFilter(UnhandledExceptionCallback);
        std::set_terminate(TerminateHandler);
    }
} // namespace Detail

DAS_DEBUG_NS_END

#else // !DAS_AUTO_MINIDUMP

#include <filesystem>
#include <functional>

DAS_DEBUG_NS_BEGIN

namespace Detail
{
    // DAS_AUTO_MINIDUMP 关闭：no-op（CrashDumper.cpp 宏关闭时不调用本函数）。
    void InstallWin32CrashHandler(
        std::function<std::filesystem::path(void*)> /*dump_path_resolver*/)
    {
    }
} // namespace Detail

DAS_DEBUG_NS_END

#endif // DAS_AUTO_MINIDUMP

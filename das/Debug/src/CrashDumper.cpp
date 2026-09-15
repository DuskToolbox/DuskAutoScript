#include <das/Debug/CrashDumper.h>

#ifdef DAS_AUTO_MINIDUMP

#include <boost/dll/runtime_symbol_info.hpp>

#include <das/Utils/BoostPathBridge.h>
#include <das/Utils/PathText.h>

#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <das/Debug/CrashDumpWriterWin32.h>
#endif

DAS_DEBUG_NS_BEGIN

namespace
{
    // 构造 dump 输出路径：{cwd}/dmp/{进程名}-{YYYYMMDD-HHMMSS}.dmp。
    // 进程名跨平台取自 boost::dll::program_location()（exe basename
    // 去扩展名）。 exception_context 此处不解析（路径只依赖进程名 +
    // 时间），交平台层在写 dump 时用其取异常记录。
    std::filesystem::path ResolveDumpPath(void* /*exception_context*/)
    {
        // 进程 exe 名可能含非 ASCII：boost→std 桥保 path 域（native 搬运
        // 零编码转换），全程不经窄串通道。
        const std::filesystem::path process_stem =
            Das::Utils::ToStdPath(boost::dll::program_location()).stem();

        const std::time_t now = std::time(nullptr);
        std::tm           local_time{};
#ifdef _WIN32
        localtime_s(&local_time, &now);
#else
        localtime_r(&now, &local_time);
#endif
        char time_buffer[24];
        std::strftime(
            time_buffer,
            sizeof(time_buffer),
            "%Y-%m-%d_%H-%M-%S",
            &local_time);

        const auto dump_dir =
            std::filesystem::current_path() / DAS_PATH_LITERAL("dmp");
        std::error_code ec;
        std::filesystem::create_directories(dump_dir, ec);

        // 文件名在 UTF-8 域一次拼好再物化：进程名可能非 ASCII，时间戳段
        // 纯 ASCII，拼接不改变编码边界。
        return dump_dir
               / Das::Utils::PathFromUtf8(
                   Das::Utils::PathToUtf8(process_stem) + std::string{"-"}
                   + time_buffer + ".dmp");
    }
} // namespace

void InstallCrashDumper()
{
#ifdef _WIN32
    Detail::InstallWin32CrashHandler(&ResolveDumpPath);
#else
    // 非 Win32 暂未实现：未来在此 #elif 调 Detail::InstallLinuxCrashHandler
    // （新增 CrashDumpWriterLinux.{h,cpp}），CrashDumper 主逻辑不动。
#endif
}

DAS_DEBUG_NS_END

#else // !DAS_AUTO_MINIDUMP

DAS_DEBUG_NS_BEGIN

// DAS_AUTO_MINIDUMP 关闭：零成本 no-op，调用方代码不用改。
void InstallCrashDumper() {}

DAS_DEBUG_NS_END

#endif // DAS_AUTO_MINIDUMP

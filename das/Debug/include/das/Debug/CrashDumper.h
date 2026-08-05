#ifndef DAS_DEBUG_CRASHDUMPER_H
#define DAS_DEBUG_CRASHDUMPER_H

#include <das/Debug/Config.h>

DAS_DEBUG_NS_BEGIN

// 安装进程级崩溃捕获：未处理 SEH 异常（0xC0000005 等）+ std::terminate
// 时，自动写 full minidump 到 工作目录/dmp/{进程名}-{时间}.dmp。
//
// 受 DAS_AUTO_MINIDUMP 宏控制（CMake option，默认 ON）；关闭时为 no-op。
// 进程名/时间/路径跨平台（boost::dll::program_location + std::filesystem），
// handler 安装与 dump 写入按平台分文件（Win32: CrashDumpWriterWin32），
// 未来 Linux 只新增 CrashDumpWriterLinux.{h,cpp}，此处不动。
//
// 必须在 main 最早期调用（任何可能崩溃的操作之前），每个进程调一次。
void InstallCrashDumper();

DAS_DEBUG_NS_END

#endif // DAS_DEBUG_CRASHDUMPER_H

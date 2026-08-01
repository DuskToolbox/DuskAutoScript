#ifndef DAS_CORE_LOGGER_LOGRECORDERRUNTIME_H
#define DAS_CORE_LOGGER_LOGRECORDERRUNTIME_H

#include <das/DasExport.h>
#include <das/DasTypes.hpp>

#include <filesystem>

namespace Das::Core
{
    // 进程级会话目录静态注册：用户可见日志（user.jsonl）与图片统一放
    // {时间戳}-{pid} 命名的会话目录下（D-88-10）。进程退出时整目录删除
    // （D-88-05b），上限与清理都按会话目录粒度。
    class LogRecorderRuntime
    {
    public:
        // 以 debug_dir 为根创建会话目录，命名 {ts}-{pid}（如
        // 20260801-123456-8841），并建 img 子目录。幂等：已初始化直接返回。
        static DAS_API DasResult
        Initialize(const std::filesystem::path& debug_dir);
        // 当前会话目录；未初始化返回空 path。
        static DAS_API std::filesystem::path SessionDir();
        // 会话目录下的 user.jsonl 路径；未初始化返回空 path。
        static DAS_API std::filesystem::path UserJsonlPath();
        // 幂等；删除整个会话目录并清空 initialized。
        static DAS_API void Shutdown();
        // 置回未初始化态（测试用）。
        static DAS_API void ResetForTest();
    };
} // namespace Das::Core

#endif // DAS_CORE_LOGGER_LOGRECORDERRUNTIME_H

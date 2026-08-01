#ifndef DAS_CORE_LOGGER_LOGRECORDERRUNTIME_H
#define DAS_CORE_LOGGER_LOGRECORDERRUNTIME_H

#include <das/DasExport.h>
#include <das/DasTypes.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace Das::Core
{
    // 进程级会话目录静态注册：用户可见日志（user.jsonl）与图片统一放
    // {时间戳}-{pid} 命名的会话目录下（D-88-10）。进程退出时整目录删除
    // （D-88-05b），上限与清理都按会话目录粒度。
    class LogRecorderRuntime
    {
    public:
        // 日志增量通知回调（D-88-01/02）：Recorder 每写盘一条 JSONL 后触发，
        // 参数为已组装完整的事件 JSON（含 api/nextCursor/entries）。值语义
        // 单例槽，跨线程仅由 recorder 调用；DasHttp 装配到 NotificationHub
        // 广播（Broadcast 自身线程安全）。未注册时空槽安全跳过。
        using LogNotifyCallback = std::function<void(const char* json_event)>;
        // 带锁写入/替换静态通知槽；传空函数可清除（Shutdown 会清空）。
        static DAS_API void SetLogNotifyCallback(LogNotifyCallback callback);
        // 组装完整事件 JSON（{"api":"api/v1/logs/get","code":0,"data":{
        // "profile":"0","nextCursor":<写盘后文件大小>,"entries":[<line_json
        // 解析对象>]}}）后调槽；空槽或解析失败安全跳过。由 Recorder::Record
        // 写盘成功后调用。
        static DAS_API void NotifyLogAppended(
            uint64_t           next_cursor,
            const std::string& line_json);
        // 以 debug_dir 为根创建会话目录，命名 {ts}-{pid}（如
        // 20260801-123456-8841），并建 img 子目录。幂等：已初始化直接返回。
        static DAS_API DasResult
        Initialize(const std::filesystem::path& debug_dir);
        // 当前会话目录；未初始化返回空 path。
        static DAS_API std::filesystem::path SessionDir();
        // 会话目录下的 user.jsonl 路径；未初始化返回空 path。
        static DAS_API std::filesystem::path UserJsonlPath();
        // 进程内图片序号（跨 recorder 实例共享），返回 "%04u" 形式的 4 位
        // 零填充序号；字典序即时间序。运行期不设缓存上限（D-88-05a）。
        static DAS_API uint32_t NextImageSeq();
        // 扫描 debug_dir 下匹配 `{ts}-{pid}` 命名的子目录，删除 pid 不存活
        // 的孤儿会话目录（D-88-05b）。remove_all 用 error_code 版 best-effort，
        // 失败仅记日志。当前进程 pid 视为存活。
        static DAS_API void CleanupOrphanSessionDirs(
            const std::filesystem::path& debug_dir);
        // 幂等；删除整个会话目录并清空 initialized。
        static DAS_API void Shutdown();
        // 置回未初始化态（测试用）。
        static DAS_API void ResetForTest();
    };
} // namespace Das::Core

#endif // DAS_CORE_LOGGER_LOGRECORDERRUNTIME_H

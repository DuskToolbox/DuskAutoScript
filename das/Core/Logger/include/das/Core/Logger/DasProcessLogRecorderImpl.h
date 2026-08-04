#ifndef DAS_CORE_LOGGER_DASPROCESSLOGRECORDERIMPL_H
#define DAS_CORE_LOGGER_DASPROCESSLOGRECORDERIMPL_H

#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasLogRecorder.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasLogEntry.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProcessLogRecorder.Implements.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Das::Core
{
    // IDasLogEntry 容器实现：插件用 Setter 填充一次，再经 Record 提交。
    // Getter 与 Setter 对称：recorder 经接口读字段，跨进程 entry proxy 经
    // getter RPC 把调用转发回插件侧 impl（避免 dynamic_cast 到具体类）。
    // 图片以 DasPtr 引用持有，Record 时编码落盘。
    class IDasLogEntryImpl final
        : public Das::ExportInterface::DasLogEntryImplBase<IDasLogEntryImpl>
    {
    public:
        DAS_IMPL SetType(Das::ExportInterface::DasLogType type) override;
        DAS_IMPL AddText(IDasReadOnlyString* text) override;
        DAS_IMPL AddImage(Das::ExportInterface::IDasImage* image) override;
        DAS_IMPL SetField(IDasReadOnlyString* key, IDasReadOnlyString* json)
            override;

        // 只读 getter：经接口暴露给 recorder（跨进程 proxy 经 RPC
        // 转发回此实现）。
        DAS_IMPL GetType(Das::ExportInterface::DasLogType* p_out_type) override;
        DAS_IMPL GetTextCount(size_t* p_out_count) override;
        DAS_IMPL GetText(size_t index, IDasReadOnlyString** pp_out_text)
            override;
        DAS_IMPL GetFieldCount(size_t* p_out_count) override;
        DAS_IMPL GetField(
            size_t               index,
            IDasReadOnlyString** pp_out_key,
            IDasReadOnlyString** pp_out_json) override;
        DAS_IMPL GetImageCount(size_t* p_out_count) override;
        DAS_IMPL GetImage(
            size_t                            index,
            Das::ExportInterface::IDasImage** pp_out_image) override;

    private:
        std::optional<Das::ExportInterface::DasLogType>           type_;
        std::vector<std::string>                                  texts_;
        std::vector<Das::DasPtr<Das::ExportInterface::IDasImage>> images_;
        std::vector<std::pair<std::string, std::string>>          fields_;
    };

    // IDasProcessLogRecorder 实现：把 IDasLogEntry 序列化为 schema 的 JSONL
    // 条目追加写盘到会话目录 user.jsonl（ofstream append + flush）。经接口
    // getter 读 entry 字段（替代 dynamic_cast 到 IDasLogEntryImpl），跨进程
    // entry proxy 与本地 impl 走同一通路。注入 IDasLogNotifier +
    // IDasLogSession： 写盘成功后 Signal(UserJsonl) 置 dirty；session 提供路径
    // / 序号。
    class DasProcessLogRecorderImpl final
        : public Das::ExportInterface::DasProcessLogRecorderImplBase<
              DasProcessLogRecorderImpl>
    {
    public:
        DasProcessLogRecorderImpl(
            Das::ExportInterface::IDasLogNotifier* notifier,
            Das::ExportInterface::IDasLogSession*  session);

        DAS_IMPL Record(Das::ExportInterface::IDasLogEntry* entry) override;

    private:
        // 借用引用：notifier 由 g_logger 持有，session 由 App.cpp 持有，二者
        // 生命周期均长于 recorder。DasPtr 让 QueryInterface/Release 路径正确
        // 而不接管所有权。
        Das::DasPtr<Das::ExportInterface::IDasLogNotifier> notifier_;
        Das::DasPtr<Das::ExportInterface::IDasLogSession>  session_;
    };
} // namespace Das::Core

#endif // DAS_CORE_LOGGER_DASPROCESSLOGRECORDERIMPL_H

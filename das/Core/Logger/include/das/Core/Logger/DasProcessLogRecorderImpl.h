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
    // 图片本 plan 只暂存引用，编码落盘在 88-02 补齐。
    class IDasLogEntryImpl final
        : public Das::ExportInterface::DasLogEntryImplBase<IDasLogEntryImpl>
    {
    public:
        DAS_IMPL SetType(Das::ExportInterface::DasLogType type) override;
        DAS_IMPL AddText(IDasReadOnlyString* text) override;
        DAS_IMPL AddImage(Das::ExportInterface::IDasImage* image) override;
        DAS_IMPL SetField(IDasReadOnlyString* key, IDasReadOnlyString* json)
            override;

        const std::optional<Das::ExportInterface::DasLogType>& GetType() const
        {
            return type_;
        }
        const std::vector<std::string>& GetTexts() const { return texts_; }
        const std::vector<Das::DasPtr<Das::ExportInterface::IDasImage>>&
        GetImages() const
        {
            return images_;
        }
        const std::vector<std::pair<std::string, std::string>>& GetFields()
            const
        {
            return fields_;
        }

    private:
        std::optional<Das::ExportInterface::DasLogType>           type_;
        std::vector<std::string>                                  texts_;
        std::vector<Das::DasPtr<Das::ExportInterface::IDasImage>> images_;
        std::vector<std::pair<std::string, std::string>>          fields_;
    };

    // IDasProcessLogRecorder 实现：把 IDasLogEntry 序列化为 D-88-11 schema 的
    // JSONL 条目追加写盘到会话目录 user.jsonl（ofstream append + flush）。
    class DasProcessLogRecorderImpl final
        : public Das::ExportInterface::DasProcessLogRecorderImplBase<
              DasProcessLogRecorderImpl>
    {
    public:
        DAS_IMPL Record(Das::ExportInterface::IDasLogEntry* entry) override;
    };
} // namespace Das::Core

#endif // DAS_CORE_LOGGER_DASPROCESSLOGRECORDERIMPL_H

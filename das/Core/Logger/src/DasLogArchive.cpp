// IDasLogArchive 实现：按字节偏移游标读取写盘的日志文件（非破坏、可回看）。
// 工厂按 DasLogArchiveSource 分发：
//   - UserJsonl：读 LogRecorderRuntime 会话目录的 user.jsonl（焦点日志）
//   - SpdlogFile：读 g_logger 的 rotating_file_sink_mt 目标文件（调试日志，
//     恢复原版设计：dynamic_cast 拿 file_sink → ifstream(filename) →
//     读前 g_logger->flush()）
// 返回的日志行为「换行分隔的拼接串」（绕开 Node NAPI 对字符串集合 out
// 的不支持），Http 层再按行拆成数组对外。两 impl 共享行对齐与归一化助手。
#include "das/_autogen/idl/wrapper/Das.ExportInterface.IDasLogArchive.Implements.hpp"

#include <das/Core/Logger/DasLogNotifierSink.h>
#include <das/Core/Logger/LogRecorderRuntime.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <spdlog/sinks/rotating_file_sink.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <string_view>

namespace
{
    // 从 offset 处读到 EOF（轮询向新只读增量）
    std::string ReadRemainder(std::ifstream& f, uint64_t offset)
    {
        f.clear();
        f.seekg(static_cast<std::streamoff>(offset));
        return std::string(
            std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>());
    }

    // chunk 内最后一个 '\n' 的下一字节位置（行尾对齐，半行留下次）；无 '\n'
    // 返回 origin
    uint64_t AlignToLastNewline(std::string_view chunk, uint64_t origin)
    {
        const auto pos = chunk.rfind('\n');
        return pos == std::string_view::npos ? origin : origin + pos + 1;
    }

    // 字节块按 '\n' 拆行（去 '\r' 与空行），再用 '\n' 拼回单一字符串
    std::string NormalizeLines(std::string_view chunk)
    {
        std::string out;
        std::size_t pos = 0;
        while (pos < chunk.size())
        {
            const auto       nl = chunk.find('\n', pos);
            std::string_view line = chunk.substr(
                pos,
                nl == std::string_view::npos ? std::string_view::npos
                                             : nl - pos);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (!line.empty())
            {
                if (!out.empty())
                {
                    out.push_back('\n');
                }
                out.append(line.data(), line.size());
            }
            if (nl == std::string_view::npos)
            {
                break;
            }
            pos = nl + 1;
        }
        return out;
    }

    DasResult EmitLines(
        const std::string&   normalized,
        IDasReadOnlyString** pp_out)
    {
        return CreateIDasReadOnlyStringFromUtf8WithLength(
            normalized.data(),
            normalized.size(),
            pp_out);
    }

    DasResult EmitEmpty(IDasReadOnlyString** pp_out)
    {
        return EmitLines(std::string{}, pp_out);
    }

    // 从 end 位置向前数 count 个行边界，返回最后 count 行的行起点；不足则 0。
    // 末尾 '\n' 是最后一行行的终止符，不计入需向前数的行数，否则会少返回一行。
    std::size_t FindLinesBack(
        std::string_view content,
        std::size_t      end,
        uint32_t         count)
    {
        std::size_t start = 0;
        uint32_t    found = 0;
        std::size_t i = end;
        if (i > 0 && content[i - 1] == '\n')
        {
            --i; // 末尾 '\n' 终止最后一行，跳过
        }
        for (; i > 0 && found < count;)
        {
            --i;
            if (content[i] == '\n')
            {
                start = i + 1;
                ++found;
            }
        }
        return found == count ? start : 0;
    }
} // namespace

// ===== UserJsonl 实现（焦点日志：读会话目录 user.jsonl）=====
// 读源固定为 LogRecorderRuntime 的会话目录 user.jsonl（焦点日志会话目录）；每条
// Record 已同步 flush，读前无需 spdlog flush。
class IDasLogArchiveUserJsonlImpl final
    : public Das::ExportInterface::DasLogArchiveImplBase<
          IDasLogArchiveUserJsonlImpl>
{
    std::filesystem::path user_jsonl_path_;

public:
    IDasLogArchiveUserJsonlImpl()
    {
        // 读源固定为会话目录 user.jsonl；构造时 runtime 未初始化则留空，
        // 读时打开失败一律返回空结果而非异常。
        user_jsonl_path_ = Das::Core::LogRecorderRuntime::UserJsonlPath();
    }

    DasResult ReadTail(
        uint32_t             lines,
        IDasReadOnlyString** pp_out_lines,
        uint64_t*            p_out_next_offset,
        uint64_t*            p_out_earliest_offset) override
    {
        DAS_UTILS_CHECK_POINTER(pp_out_lines);
        DAS_UTILS_CHECK_POINTER(p_out_next_offset);
        DAS_UTILS_CHECK_POINTER(p_out_earliest_offset);
        *pp_out_lines = nullptr;
        *p_out_next_offset = 0;
        *p_out_earliest_offset = 0;
        if (user_jsonl_path_.empty())
            return EmitEmpty(pp_out_lines);

        std::ifstream f(user_jsonl_path_, std::ios::binary);
        if (!f)
        {
            return EmitEmpty(pp_out_lines);
        }
        const std::string content = ReadRemainder(f, 0);

        const std::size_t start = FindLinesBack(content, content.size(), lines);
        *p_out_earliest_offset = start;
        const uint64_t next = AlignToLastNewline(
            {content.data() + start, content.size() - start},
            start);
        *p_out_next_offset = next;
        return EmitLines(
            NormalizeLines(
                {content.data() + start,
                 static_cast<std::size_t>(next - start)}),
            pp_out_lines);
    }

    DasResult ReadAfter(
        uint64_t             offset,
        IDasReadOnlyString** pp_out_lines,
        uint64_t*            p_out_next_offset) override
    {
        DAS_UTILS_CHECK_POINTER(pp_out_lines);
        DAS_UTILS_CHECK_POINTER(p_out_next_offset);
        *pp_out_lines = nullptr;
        *p_out_next_offset = offset;
        if (user_jsonl_path_.empty())
            return EmitEmpty(pp_out_lines);

        std::ifstream f(user_jsonl_path_, std::ios::binary);
        if (!f)
        {
            return EmitEmpty(pp_out_lines);
        }
        f.seekg(0, std::ios::end);
        const uint64_t size = static_cast<uint64_t>(f.tellg());
        if (offset > size)
        {
            *p_out_next_offset = 0;
            return EmitEmpty(pp_out_lines);
        } // 轮转：UI 重发 tail
        if (offset == size)
        {
            return EmitEmpty(pp_out_lines); // 无新内容
        }

        const std::string chunk = ReadRemainder(f, offset);
        const uint64_t    next = AlignToLastNewline(chunk, offset);
        *p_out_next_offset = next;
        return EmitLines(
            NormalizeLines(
                {chunk.data(), static_cast<std::size_t>(next - offset)}),
            pp_out_lines);
    }

    DasResult ReadBefore(
        uint64_t             offset,
        uint32_t             lines,
        IDasReadOnlyString** pp_out_lines,
        uint64_t*            p_out_next_offset,
        bool*                p_out_eof) override
    {
        DAS_UTILS_CHECK_POINTER(pp_out_lines);
        DAS_UTILS_CHECK_POINTER(p_out_next_offset);
        DAS_UTILS_CHECK_POINTER(p_out_eof);
        *pp_out_lines = nullptr;
        *p_out_next_offset = offset;
        *p_out_eof = true;
        if (user_jsonl_path_.empty())
            return EmitEmpty(pp_out_lines);

        std::ifstream f(user_jsonl_path_, std::ios::binary);
        if (!f)
        {
            return EmitEmpty(pp_out_lines);
        }
        const std::string content = ReadRemainder(f, 0);

        const uint64_t size = content.size();
        if (offset > size)
        {
            *p_out_next_offset = 0;
            *p_out_eof = true;
            return EmitEmpty(pp_out_lines);
        }
        const auto end = static_cast<std::size_t>(offset);
        const auto start = FindLinesBack(content, end, lines);
        *p_out_next_offset = start;
        *p_out_eof = (start == 0);
        return EmitLines(
            NormalizeLines({content.data() + start, end - start}),
            pp_out_lines);
    }
};

// ===== SpdlogFile 实现（调试日志：读 g_logger 的 rotating_file_sink_mt
// 目标文件）===== 恢复原版设计：ctor 遍历 g_logger->sinks() dynamic_cast
// 拿 file_sink 指针 （sinks vector 固定，指针稳定）；ReadTail/After/Before 读前
// g_logger->flush() （spdlog FILE* 缓冲默认不自动 flush，未落盘的日志 ifstream
// 读不到）；轮转时 offset 大于当前文件大小 → 返回 next=0 让 UI 重发 tail（与
// 与原版一致）。
class IDasLogArchiveSpdlogFileImpl final
    : public Das::ExportInterface::DasLogArchiveImplBase<
          IDasLogArchiveSpdlogFileImpl>
{
    spdlog::sinks::rotating_file_sink_mt* file_sink_ = nullptr;

public:
    IDasLogArchiveSpdlogFileImpl()
    {
        // sinks vector 初始化后不变，构造时缓存 file_sink
        // 指针即可（sinks vector 构造后不变）。
        if (Das::Core::g_logger)
        {
            for (const auto& sink : Das::Core::g_logger->sinks())
            {
                if (auto* fs =
                        dynamic_cast<spdlog::sinks::rotating_file_sink_mt*>(
                            sink.get()))
                {
                    file_sink_ = fs;
                }
            }
        }
    }

    DasResult ReadTail(
        uint32_t             lines,
        IDasReadOnlyString** pp_out_lines,
        uint64_t*            p_out_next_offset,
        uint64_t*            p_out_earliest_offset) override
    {
        DAS_UTILS_CHECK_POINTER(pp_out_lines);
        DAS_UTILS_CHECK_POINTER(p_out_next_offset);
        DAS_UTILS_CHECK_POINTER(p_out_earliest_offset);
        *pp_out_lines = nullptr;
        *p_out_next_offset = 0;
        *p_out_earliest_offset = 0;
        if (file_sink_ == nullptr)
        {
            return EmitEmpty(pp_out_lines);
        }

        // 读前强制落盘：spdlog file_sink 的 FILE* 缓冲默认不自动
        // flush，未落盘的日志 ifstream 读不到。读前 flush
        // 一次把缓冲冲到磁盘，UI 轮询才能拿到最新。
        Das::Core::g_logger->flush();
        std::ifstream f(file_sink_->filename(), std::ios::binary);
        if (!f)
        {
            return EmitEmpty(pp_out_lines);
        }
        const std::string content = ReadRemainder(f, 0);

        const std::size_t start = FindLinesBack(content, content.size(), lines);
        *p_out_earliest_offset = start;
        const uint64_t next = AlignToLastNewline(
            {content.data() + start, content.size() - start},
            start);
        *p_out_next_offset = next;
        return EmitLines(
            NormalizeLines(
                {content.data() + start,
                 static_cast<std::size_t>(next - start)}),
            pp_out_lines);
    }

    DasResult ReadAfter(
        uint64_t             offset,
        IDasReadOnlyString** pp_out_lines,
        uint64_t*            p_out_next_offset) override
    {
        DAS_UTILS_CHECK_POINTER(pp_out_lines);
        DAS_UTILS_CHECK_POINTER(p_out_next_offset);
        *pp_out_lines = nullptr;
        *p_out_next_offset = offset;
        if (file_sink_ == nullptr)
        {
            return EmitEmpty(pp_out_lines);
        }

        Das::Core::g_logger->flush(); // 读前落盘（同上）
        std::ifstream f(file_sink_->filename(), std::ios::binary);
        if (!f)
        {
            return EmitEmpty(pp_out_lines);
        }
        f.seekg(0, std::ios::end);
        const uint64_t size = static_cast<uint64_t>(f.tellg());
        if (offset > size)
        {
            *p_out_next_offset = 0;
            return EmitEmpty(pp_out_lines);
        } // 轮转：UI 重发 tail
        if (offset == size)
        {
            return EmitEmpty(pp_out_lines); // 无新内容
        }

        const std::string chunk = ReadRemainder(f, offset);
        const uint64_t    next = AlignToLastNewline(chunk, offset);
        *p_out_next_offset = next;
        return EmitLines(
            NormalizeLines(
                {chunk.data(), static_cast<std::size_t>(next - offset)}),
            pp_out_lines);
    }

    DasResult ReadBefore(
        uint64_t             offset,
        uint32_t             lines,
        IDasReadOnlyString** pp_out_lines,
        uint64_t*            p_out_next_offset,
        bool*                p_out_eof) override
    {
        DAS_UTILS_CHECK_POINTER(pp_out_lines);
        DAS_UTILS_CHECK_POINTER(p_out_next_offset);
        DAS_UTILS_CHECK_POINTER(p_out_eof);
        *pp_out_lines = nullptr;
        *p_out_next_offset = offset;
        *p_out_eof = true;
        if (file_sink_ == nullptr)
        {
            return EmitEmpty(pp_out_lines);
        }

        Das::Core::g_logger->flush(); // 读前落盘（同上）
        std::ifstream f(file_sink_->filename(), std::ios::binary);
        if (!f)
        {
            return EmitEmpty(pp_out_lines);
        }
        const std::string content = ReadRemainder(f, 0);

        const uint64_t size = content.size();
        if (offset > size)
        {
            *p_out_next_offset = 0;
            *p_out_eof = true;
            return EmitEmpty(pp_out_lines);
        }
        const auto end = static_cast<std::size_t>(offset);
        const auto start = FindLinesBack(content, end, lines);
        *p_out_next_offset = start;
        *p_out_eof = (start == 0);
        return EmitLines(
            NormalizeLines({content.data() + start, end - start}),
            pp_out_lines);
    }
};

DasResult CreateIDasLogArchive(
    Das::ExportInterface::DasLogArchiveSource source,
    Das::ExportInterface::IDasLogArchive**    pp_out_archive)
{
    DAS_UTILS_CHECK_POINTER(pp_out_archive);
    try
    {
        switch (source)
        {
        case Das::ExportInterface::DAS_LOG_ARCHIVE_SOURCE_USER_JSONL:
        {
            auto* p = new IDasLogArchiveUserJsonlImpl{};
            *pp_out_archive = p;
            p->AddRef();
            return DAS_S_OK;
        }
        case Das::ExportInterface::DAS_LOG_ARCHIVE_SOURCE_SPDLOG_FILE:
        {
            auto* p = new IDasLogArchiveSpdlogFileImpl{};
            *pp_out_archive = p;
            p->AddRef();
            return DAS_S_OK;
        }
        default:
            return DAS_E_INVALID_ARGUMENT;
        }
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// GetDasLogNotifier：dynamic_cast g_logger->sinks() 拿固定 DasLogNotifierSink，
// 返回借用 IDasLogNotifier 引用（借用引用语义）。sink
// 未装配（早期初始化或单测环境） 返回 DAS_E_NOT_FOUND 与 null。
DasResult GetDasLogNotifier(
    Das::ExportInterface::IDasLogNotifier** pp_out_notifier)
{
    DAS_UTILS_CHECK_POINTER(pp_out_notifier);
    *pp_out_notifier = nullptr;
    if (!Das::Core::g_logger)
    {
        return DAS_E_NOT_FOUND;
    }
    for (const auto& sink : Das::Core::g_logger->sinks())
    {
        if (auto* notifier_sink =
                dynamic_cast<Das::Core::DasLogNotifierSink*>(sink.get()))
        {
            *pp_out_notifier =
                static_cast<Das::ExportInterface::IDasLogNotifier*>(
                    notifier_sink);
            return DAS_S_OK;
        }
    }
    return DAS_E_NOT_FOUND;
}

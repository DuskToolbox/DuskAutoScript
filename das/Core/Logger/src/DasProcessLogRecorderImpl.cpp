#include <das/Core/Logger/DasProcessLogRecorderImpl.h>

#include <das/Core/Logger/LogRecorderRuntime.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/OcvWrapper/ImageSaver.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <new>
#include <string>
#include <string_view>

namespace Das::Core
{
    namespace
    {
        // DasLogType -> 小写字符串（D-88-11 schema 的 type 取值）。
        const char* LogTypeToString(Das::ExportInterface::DasLogType type)
        {
            switch (type)
            {
            case Das::ExportInterface::DAS_LOG_TYPE_SUCCESS:
                return "success";
            case Das::ExportInterface::DAS_LOG_TYPE_WARNING:
                return "warning";
            case Das::ExportInterface::DAS_LOG_TYPE_ERROR:
                return "error";
            case Das::ExportInterface::DAS_LOG_TYPE_AGENT:
                return "agent";
            case Das::ExportInterface::DAS_LOG_TYPE_FOCUS:
                return "focus";
            case Das::ExportInterface::DAS_LOG_TYPE_INFO:
            default:
                return "info";
            }
        }

        int64_t NowMsSinceEpoch()
        {
            const auto now = std::chrono::system_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch())
                .count();
        }

        std::string JoinTexts(const std::vector<std::string>& texts)
        {
            std::string out;
            for (const auto& text : texts)
            {
                if (!out.empty())
                {
                    out.push_back('\n');
                }
                out += text;
            }
            return out;
        }
    } // namespace

    // ----------------------------------------------------------------
    // IDasLogEntry

    DasResult IDasLogEntryImpl::SetType(Das::ExportInterface::DasLogType type)
    {
        if (type < Das::ExportInterface::DAS_LOG_TYPE_INFO
            || type > Das::ExportInterface::DAS_LOG_TYPE_FOCUS)
        {
            return DAS_E_INVALID_ENUM;
        }
        type_ = type;
        return DAS_S_OK;
    }

    DasResult IDasLogEntryImpl::AddText(IDasReadOnlyString* text)
    {
        DAS_UTILS_CHECK_POINTER(text);
        const char* p_utf8 = nullptr;
        const auto  result = text->GetUtf8(&p_utf8);
        if (result < 0)
        {
            return result;
        }
        if (p_utf8 == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        texts_.emplace_back(p_utf8);
        return DAS_S_OK;
    }

    DasResult IDasLogEntryImpl::AddImage(Das::ExportInterface::IDasImage* image)
    {
        DAS_UTILS_CHECK_POINTER(image);
        // DasPtr 构造 AddRef，本 plan 只暂存引用（D-88-06），不落盘。
        images_.emplace_back(image);
        return DAS_S_OK;
    }

    DasResult IDasLogEntryImpl::SetField(
        IDasReadOnlyString* key,
        IDasReadOnlyString* json)
    {
        DAS_UTILS_CHECK_POINTER(key);
        DAS_UTILS_CHECK_POINTER(json);
        const char* p_key = nullptr;
        const char* p_json = nullptr;
        const auto  key_result = key->GetUtf8(&p_key);
        if (key_result < 0)
        {
            return key_result;
        }
        const auto json_result = json->GetUtf8(&p_json);
        if (json_result < 0)
        {
            return json_result;
        }
        if (p_key == nullptr || p_json == nullptr || p_key[0] == '\0')
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        if (!DAS::Utils::ParseYyjsonFromString(p_json))
        {
            return DAS_E_INVALID_JSON;
        }
        fields_.emplace_back(std::string{p_key}, std::string{p_json});
        return DAS_S_OK;
    }

    // ------------------------------------------------------
    // DasProcessLogRecorder

    DasResult DasProcessLogRecorderImpl::Record(
        Das::ExportInterface::IDasLogEntry* entry)
    {
        DAS_UTILS_CHECK_POINTER(entry);

        auto* entry_impl = dynamic_cast<IDasLogEntryImpl*>(entry);
        if (entry_impl == nullptr)
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        const auto user_jsonl_path = LogRecorderRuntime::UserJsonlPath();
        if (user_jsonl_path.empty())
        {
            return DAS_E_OBJECT_NOT_INIT;
        }

        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("time")] = NowMsSinceEpoch();
        const auto type = entry_impl->GetType().value_or(
            Das::ExportInterface::DAS_LOG_TYPE_INFO);
        (*obj.as_object())[std::string_view("type")] = std::make_pair(
            std::string_view(LogTypeToString(type)),
            yyjson::copy_string);
        const auto joined_text = JoinTexts(entry_impl->GetTexts());
        (*obj.as_object())[std::string_view("text")] =
            std::make_pair(std::string_view(joined_text), yyjson::copy_string);

        // 图片编码落盘（D-88-04a）：每个 IDasImage 经 OcvWrapper 的
        // ImageSaver（JPG 质量 85）写到会话目录 img/{seq}.jpg，imageRefs
        // 填相对路径。单个图片失败仅记日志跳过，不影响文本条目。运行期
        // 不设缓存上限（D-88-05a）。
        auto       image_refs = DAS::Utils::MakeYyjsonArray();
        const auto session_dir = LogRecorderRuntime::SessionDir();
        auto       image_refs_array = image_refs.as_array();
        for (const auto& image : entry_impl->GetImages())
        {
            const auto seq = LogRecorderRuntime::NextImageSeq();
            char       file_name[32]{};
            std::snprintf(file_name, sizeof file_name, "img/%04u.jpg", seq);
            const auto rc = Das::Core::OcvWrapper::ImageSaver::SaveToFile(
                image.Get(),
                Das::ExportInterface::DAS_IMAGE_FORMAT_JPG,
                85,
                session_dir / file_name);
            if (DAS::IsFailed(rc))
            {
                DAS_CORE_LOG_ERROR(
                    "Record: failed to save image {}: result={}",
                    file_name,
                    rc);
                continue;
            }
            if (image_refs_array)
            {
                image_refs_array->emplace_back(
                    std::string_view{file_name},
                    yyjson::copy_string);
            }
        }
        (*obj.as_object())[std::string_view("imageRefs")] = image_refs;

        auto fields_obj = DAS::Utils::MakeYyjsonObject();
        for (const auto& [key, json] : entry_impl->GetFields())
        {
            auto parsed = DAS::Utils::ParseYyjsonFromString(json);
            if (!parsed)
            {
                continue; // 解析失败跳过该项
            }
            (*fields_obj.as_object())[std::string_view(key)] =
                std::move(*parsed);
        }
        (*obj.as_object())[std::string_view("fields")] = fields_obj;

        auto line = DAS::Utils::SerializeYyjsonValue(obj);
        if (!line)
        {
            return DAS_E_INVALID_JSON;
        }

        std::ofstream output{user_jsonl_path, std::ios::app | std::ios::binary};
        if (!output)
        {
            return DAS_E_INVALID_FILE;
        }
        output << *line << '\n';
        output.flush();
        if (!output)
        {
            return DAS_E_INVALID_FILE;
        }

        // 日志增量通知（D-88-01/02）：写盘成功后把 newCursor 与新条目交给
        // LogRecorderRuntime 组装完整事件 JSON 并触发通知槽（DasHttp 侧装配
        // 到 NotificationHub 广播）。nextCursor = 写盘后文件大小，与 archive
        // 的 AlignToLastNewline 行尾对齐一致。空槽安全跳过，装配前的事件由
        // HTTP 轮询兜底。
        std::error_code size_ec;
        const auto      next_cursor =
            std::filesystem::file_size(user_jsonl_path, size_ec);
        if (!size_ec)
        {
            LogRecorderRuntime::NotifyLogAppended(next_cursor, *line);
        }
        return DAS_S_OK;
    }

} // namespace Das::Core

// ------------------------------------------------------------ C ABI 工厂

DasResult CreateIDasProcessLogRecorder(
    Das::ExportInterface::IDasProcessLogRecorder** pp_out_recorder)
{
    DAS_UTILS_CHECK_POINTER(pp_out_recorder);
    try
    {
        auto* p = new Das::Core::DasProcessLogRecorderImpl{};
        *pp_out_recorder = p;
        p->AddRef();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

#include <das/Core/Logger/DasProcessLogRecorderImpl.h>

#include <das/Core/Logger/Logger.h>
#include <das/Core/OcvWrapper/ImageSaver.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <string_view>

namespace Das::Core
{
    namespace
    {
        // DasLogType -> 小写字符串（schema 的 type 取值）。
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
            case Das::ExportInterface::DAS_LOG_TYPE_STRUCTURAL:
                return "structural";
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

        // 从 IDasLogEntry 接口读所有 text，按 '\n' 拼接成单串。
        // 跨进程 entry 经 getter RPC 读（recorder 持 entry 期间 plugin
        // 不能死）。
        std::string JoinEntryTexts(Das::ExportInterface::IDasLogEntry* entry)
        {
            size_t text_count = 0;
            if (DAS::IsFailed(entry->GetTextCount(&text_count)))
            {
                return {};
            }
            std::string out;
            for (size_t index = 0; index < text_count; ++index)
            {
                Das::DasPtr<IDasReadOnlyString> text_str;
                if (DAS::IsFailed(entry->GetText(index, text_str.Put())))
                {
                    continue;
                }
                if (!text_str)
                {
                    continue;
                }
                const char* utf8 = nullptr;
                if (DAS::IsFailed(text_str->GetUtf8(&utf8)) || utf8 == nullptr)
                {
                    continue;
                }
                if (!out.empty())
                {
                    out.push_back('\n');
                }
                out += utf8;
            }
            return out;
        }

        // 读 session_dir 路径（IDasReadOnlyString → std::filesystem::path）。
        std::filesystem::path ReadSessionDirectory(
            Das::ExportInterface::IDasLogSession* session)
        {
            if (session == nullptr)
            {
                return {};
            }
            Das::DasPtr<IDasReadOnlyString> directory_str;
            if (DAS::IsFailed(session->GetSessionDir(directory_str.Put())))
            {
                return {};
            }
            if (!directory_str)
            {
                return {};
            }
            const char* utf8 = nullptr;
            if (DAS::IsFailed(directory_str->GetUtf8(&utf8)) || utf8 == nullptr)
            {
                return {};
            }
            return std::filesystem::path{
                reinterpret_cast<const char8_t*>(utf8)};
        }

        std::filesystem::path ReadUserJsonlPath(
            Das::ExportInterface::IDasLogSession* session)
        {
            if (session == nullptr)
            {
                return {};
            }
            Das::DasPtr<IDasReadOnlyString> path_str;
            if (DAS::IsFailed(session->GetUserJsonlPath(path_str.Put())))
            {
                return {};
            }
            if (!path_str)
            {
                return {};
            }
            const char* utf8 = nullptr;
            if (DAS::IsFailed(path_str->GetUtf8(&utf8)) || utf8 == nullptr)
            {
                return {};
            }
            return std::filesystem::path{
                reinterpret_cast<const char8_t*>(utf8)};
        }

        DasResult MakeReadOnlyStringFromUtf8(
            const std::string&   value,
            IDasReadOnlyString** pp_out)
        {
            return CreateIDasReadOnlyStringFromUtf8(value.c_str(), pp_out);
        }
    } // namespace

    // ----------------------------------------------------------------
    // IDasLogEntry

    DasResult IDasLogEntryImpl::SetType(Das::ExportInterface::DasLogType type)
    {
        if (type < Das::ExportInterface::DAS_LOG_TYPE_INFO
            || type > Das::ExportInterface::DAS_LOG_TYPE_STRUCTURAL)
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
        // DasPtr 构造 AddRef，recorder 持引用读字段，编码落盘在 Record 中完成。
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

    DasResult IDasLogEntryImpl::GetType(
        Das::ExportInterface::DasLogType* p_out_type)
    {
        DAS_UTILS_CHECK_POINTER(p_out_type);
        if (!type_.has_value())
        {
            return DAS_E_OBJECT_NOT_INIT;
        }
        *p_out_type = *type_;
        return DAS_S_OK;
    }

    DasResult IDasLogEntryImpl::GetTextCount(size_t* p_out_count)
    {
        DAS_UTILS_CHECK_POINTER(p_out_count);
        *p_out_count = texts_.size();
        return DAS_S_OK;
    }

    DasResult IDasLogEntryImpl::GetText(
        size_t               index,
        IDasReadOnlyString** pp_out_text)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_text);
        *pp_out_text = nullptr;
        if (index >= texts_.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }
        return MakeReadOnlyStringFromUtf8(texts_[index], pp_out_text);
    }

    DasResult IDasLogEntryImpl::GetFieldCount(size_t* p_out_count)
    {
        DAS_UTILS_CHECK_POINTER(p_out_count);
        *p_out_count = fields_.size();
        return DAS_S_OK;
    }

    DasResult IDasLogEntryImpl::GetField(
        size_t               index,
        IDasReadOnlyString** pp_out_key,
        IDasReadOnlyString** pp_out_json)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_key);
        DAS_UTILS_CHECK_POINTER(pp_out_json);
        *pp_out_key = nullptr;
        *pp_out_json = nullptr;
        if (index >= fields_.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }
        const auto& [key, json] = fields_[index];
        const auto key_result = MakeReadOnlyStringFromUtf8(key, pp_out_key);
        if (DAS::IsFailed(key_result))
        {
            return key_result;
        }
        const auto json_result = MakeReadOnlyStringFromUtf8(json, pp_out_json);
        if (DAS::IsFailed(json_result))
        {
            return json_result;
        }
        return DAS_S_OK;
    }

    DasResult IDasLogEntryImpl::GetImageCount(size_t* p_out_count)
    {
        DAS_UTILS_CHECK_POINTER(p_out_count);
        *p_out_count = images_.size();
        return DAS_S_OK;
    }

    DasResult IDasLogEntryImpl::GetImage(
        size_t                            index,
        Das::ExportInterface::IDasImage** pp_out_image)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_image);
        *pp_out_image = nullptr;
        if (index >= images_.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }
        auto* raw = images_[index].Get();
        if (raw == nullptr)
        {
            return DAS_E_OBJECT_NOT_INIT;
        }
        raw->AddRef();
        *pp_out_image = raw;
        return DAS_S_OK;
    }

    // ------------------------------------------------------
    // DasProcessLogRecorder

    DasProcessLogRecorderImpl::DasProcessLogRecorderImpl(
        Das::ExportInterface::IDasLogNotifier* notifier,
        Das::ExportInterface::IDasLogSession*  session)
    {
        if (notifier != nullptr)
        {
            notifier_ =
                Das::DasPtr<Das::ExportInterface::IDasLogNotifier>(notifier);
        }
        if (session != nullptr)
        {
            session_ =
                Das::DasPtr<Das::ExportInterface::IDasLogSession>(session);
        }
    }

    DasResult DasProcessLogRecorderImpl::Record(
        Das::ExportInterface::IDasLogEntry* entry)
    {
        DAS_UTILS_CHECK_POINTER(entry);

        // 经接口 getter 读字段（替代 dynamic_cast 到 IDasLogEntryImpl）：
        // 跨进程 entry 是 DasLogEntryProxy（by-reference），cast 到具体 impl
        // 返 nullptr 会丢日志；接口 getter 经 RPC 转发回插件侧 impl 读真值。
        Das::ExportInterface::DasLogType type =
            Das::ExportInterface::DAS_LOG_TYPE_INFO;
        {
            const auto type_result = entry->GetType(&type);
            if (DAS::IsFailed(type_result)
                && type_result != DAS_E_OBJECT_NOT_INIT)
            {
                return type_result;
            }
            // GetType 未设置时返回 OBJECT_NOT_INIT，此时保留 INFO 默认值。
        }

        const auto user_jsonl_path = ReadUserJsonlPath(session_.Get());
        if (user_jsonl_path.empty())
        {
            return DAS_E_OBJECT_NOT_INIT;
        }

        auto obj = DAS::Utils::MakeYyjsonObject();
        (*obj.as_object())[std::string_view("time")] = NowMsSinceEpoch();
        (*obj.as_object())[std::string_view("type")] = std::make_pair(
            std::string_view(LogTypeToString(type)),
            yyjson::copy_string);
        const auto joined_text = JoinEntryTexts(entry);
        (*obj.as_object())[std::string_view("text")] =
            std::make_pair(std::string_view(joined_text), yyjson::copy_string);

        // 图片编码落盘：每个 IDasImage 经 OcvWrapper 的 ImageSaver（JPG 质量
        // 85）写到会话目录 img/{seq}.jpg，imageRefs 填相对路径。单个图片失败
        // 仅记日志跳过，不影响文本条目。
        auto       image_refs = DAS::Utils::MakeYyjsonArray();
        const auto session_dir = ReadSessionDirectory(session_.Get());
        auto       image_refs_array = image_refs.as_array();
        size_t     image_count = 0;
        if (DAS::IsOk(entry->GetImageCount(&image_count)))
        {
            for (size_t index = 0; index < image_count; ++index)
            {
                Das::DasPtr<Das::ExportInterface::IDasImage> image;
                if (DAS::IsFailed(entry->GetImage(index, image.Put())))
                {
                    continue;
                }
                if (!image)
                {
                    continue;
                }
                uint32_t seq = 0;
                if (DAS::IsFailed(session_->NextImageSeq(&seq)))
                {
                    continue;
                }
                char file_name[32]{};
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
        }
        (*obj.as_object())[std::string_view("imageRefs")] = image_refs;

        auto   fields_obj = DAS::Utils::MakeYyjsonObject();
        size_t field_count = 0;
        if (DAS::IsOk(entry->GetFieldCount(&field_count)))
        {
            for (size_t index = 0; index < field_count; ++index)
            {
                Das::DasPtr<IDasReadOnlyString> key_str;
                Das::DasPtr<IDasReadOnlyString> json_str;
                if (DAS::IsFailed(
                        entry->GetField(index, key_str.Put(), json_str.Put())))
                {
                    continue;
                }
                if (!key_str || !json_str)
                {
                    continue;
                }
                const char* key_utf8 = nullptr;
                const char* json_utf8 = nullptr;
                if (DAS::IsFailed(key_str->GetUtf8(&key_utf8))
                    || key_utf8 == nullptr)
                {
                    continue;
                }
                if (DAS::IsFailed(json_str->GetUtf8(&json_utf8))
                    || json_utf8 == nullptr)
                {
                    continue;
                }
                auto parsed = DAS::Utils::ParseYyjsonFromString(json_utf8);
                if (!parsed)
                {
                    continue; // 解析失败跳过该项
                }
                (*fields_obj.as_object())[std::string_view(key_utf8)] =
                    std::move(*parsed);
            }
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

        // 写盘成功后置 UserJsonl dirty flag（Http 16ms 定时器 Consume →
        // hub->Broadcast api/v1/logs/get tick，前端 Refresh→ReadAfter 拉新）。
        // notifier 为空时（测试或未装配）安全跳过——Http 轮询不依赖此 path。
        if (notifier_)
        {
            (void)notifier_->Signal(
                Das::ExportInterface::DAS_LOG_CHANNEL_USER_JSONL);
        }
        return DAS_S_OK;
    }

} // namespace Das::Core

// ------------------------------------------------------------ C ABI 工厂

DasResult CreateIDasLogEntry(Das::ExportInterface::IDasLogEntry** pp_out_entry)
{
    DAS_UTILS_CHECK_POINTER(pp_out_entry);
    try
    {
        auto* p = new Das::Core::IDasLogEntryImpl{};
        *pp_out_entry = p;
        p->AddRef();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult CreateIDasProcessLogRecorder(
    Das::ExportInterface::IDasLogNotifier*         notifier,
    Das::ExportInterface::IDasLogSession*          session,
    Das::ExportInterface::IDasProcessLogRecorder** pp_out_recorder)
{
    DAS_UTILS_CHECK_POINTER(pp_out_recorder);
    try
    {
        auto* p = new Das::Core::DasProcessLogRecorderImpl{notifier, session};
        *pp_out_recorder = p;
        p->AddRef();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

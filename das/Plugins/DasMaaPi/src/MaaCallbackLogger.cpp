#include "MaaCallbackLogger.h"

#include "PiHttpClient.h"

#include <das/DasString.hpp>
#include <das/Utils/PathText.h>
#include <das/Utils/fmt.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <utility>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        // URL/文件响应体大小上限（1 MiB = 1048576 字节）。恶意 PI 资源可能
        // 指向流式 GB 端点在 BeastHttpClient 5s 超时窗口内拉满 RAM；截断首
        // 1 MiB 保证单条 focus content 内存占用硬性上限。镜像协议规范要求
        // 的「URL/focus content」合理体量（典型 release note / changelog
        // 100 KB 以内）。
        constexpr std::size_t MAX_FOCUS_CONTENT_BYTES = 1U << 20;
        // 从 yyjson 对象取 display 渠道：string → 单元素；string 数组 → 各
        // 元素；缺省/非法 → 默认 ["log"]。
        void AppendDisplayChannels(
            const yyjson::value&      display,
            std::vector<std::string>& out_channels)
        {
            if (auto channel = display.as_string())
            {
                out_channels.emplace_back(channel->data(), channel->size());
                return;
            }
            if (auto array = display.as_array())
            {
                for (auto it = array->begin(); it != array->end(); ++it)
                {
                    if (auto channel = it->as_string())
                    {
                        out_channels.emplace_back(
                            channel->data(),
                            channel->size());
                    }
                }
                if (!out_channels.empty())
                {
                    return;
                }
            }
            out_channels.emplace_back("log");
        }

        // 组装 IDasLogEntry（C ABI 工厂，跨 DLL 安全）并填充 FOCUS 条目。
        bool BuildFocusEntry(
            Das::DasPtr<Das::ExportInterface::IDasLogEntry>&    out_entry,
            const std::string&                                  text,
            const Das::DasPtr<Das::ExportInterface::IDasImage>& image,
            const std::string& display_channels_json)
        {
            Das::DasPtr<Das::ExportInterface::IDasLogEntry> entry;
            if (DAS::IsFailed(CreateIDasLogEntry(entry.Put())))
            {
                return false;
            }
            if (DAS::IsFailed(entry->SetType(
                    Das::ExportInterface::DAS_LOG_TYPE_STRUCTURAL)))
            {
                return false;
            }
            DasU8StringOnStack text_string{text.c_str()};
            if (DAS::IsFailed(entry->AddText(&text_string)))
            {
                return false;
            }
            if (image)
            {
                if (DAS::IsFailed(entry->AddImage(image.Get())))
                {
                    return false;
                }
            }
            DasU8StringOnStack key_string{"displayChannels"};
            DasU8StringOnStack value_string{display_channels_json.c_str()};
            if (DAS::IsFailed(entry->SetField(&key_string, &value_string)))
            {
                return false;
            }
            out_entry = std::move(entry);
            return true;
        }
    } // namespace

    std::optional<FocusTemplateResult> ParseFocusTemplate(
        const yyjson::value& details,
        std::string_view     message)
    {
        auto details_obj = details.as_object();
        if (!details_obj)
        {
            return std::nullopt;
        }
        // object_get 对缺失 key 抛 std::out_of_range，先用 contains 守卫
        // （T-88-11：不信任 details_json 结构）。
        if (!details_obj->contains("focus"))
        {
            return std::nullopt;
        }
        auto focus_value = (*details_obj)[std::string_view("focus")];
        auto focus_obj = focus_value.as_object();
        if (!focus_obj || !focus_obj->contains(message))
        {
            return std::nullopt;
        }
        auto                entry_value = (*focus_obj)[message];
        FocusTemplateResult result;
        // 字符串简写：content 即字符串，渠道默认 log。
        if (auto content = entry_value.as_string())
        {
            result.content.assign(content->data(), content->size());
            result.channels.emplace_back("log");
            return result;
        }
        // 对象完整写法：content + display（string 或数组）。
        auto entry_obj = entry_value.as_object();
        if (!entry_obj)
        {
            return std::nullopt;
        }
        auto content_value = (*entry_obj)[std::string_view("content")];
        auto content = content_value.as_string();
        if (!content)
        {
            return std::nullopt;
        }
        result.content.assign(content->data(), content->size());
        if (!entry_obj->contains("display"))
        {
            result.channels.emplace_back("log");
            return result;
        }
        auto display_value = (*entry_obj)[std::string_view("display")];
        if (display_value.is_null())
        {
            result.channels.emplace_back("log");
            return result;
        }
        AppendDisplayChannels(display_value, result.channels);
        return result;
    }

    std::string ReplaceFocusPlaceholders(
        std::string_view     text,
        const yyjson::value& details)
    {
        // std::regex 需要双向迭代器；string_view 的迭代器是裸 const char*，
        // 与 libstdc++ 的 __wrap_iter 不匹配，先拷贝为 std::string。
        static const std::regex     placeholder_pattern{R"(\{(\w+)\})"};
        const std::string           source{text};
        std::string                 result;
        auto                        details_obj = details.as_object();
        std::string::const_iterator cursor = source.begin();
        for (std::sregex_iterator
                 it{source.begin(), source.end(), placeholder_pattern},
             end{};
             it != end;
             ++it)
        {
            const auto& match = *it;
            result.append(cursor, match[0].first);
            const std::string key = match[1].str();
            if (key == "image")
            {
                // {image} 由截图通路单独处理，此处保留原样。
                result.append(match[0].first, match[0].second);
            }
            else if (details_obj && details_obj->contains(key))
            {
                // String(value) 语义：字符串直接取，数字/bool
                // 转十进制文本；结构值（对象/数组）不替换，占位符原样保留。
                auto value = (*details_obj)[key];
                if (auto s = value.as_string())
                {
                    result.append(s->data(), s->size());
                }
                else if (auto signed_integer = value.as_sint())
                {
                    result.append(std::to_string(*signed_integer));
                }
                else if (auto unsigned_integer = value.as_uint())
                {
                    result.append(std::to_string(*unsigned_integer));
                }
                else if (auto real = value.as_real())
                {
                    result.append(std::to_string(*real));
                }
                else if (auto boolean = value.as_bool())
                {
                    result.append(*boolean ? "true" : "false");
                }
                else
                {
                    result.append(match[0].first, match[0].second);
                }
            }
            else
            {
                result.append(match[0].first, match[0].second);
            }
            cursor = match[0].second;
        }
        result.append(cursor, source.end());
        return result;
    }

    std::string ResolveFocusContent(
        std::string_view                content,
        const MaaCallbackLoggerContext& context)
    {
        if (content.empty())
        {
            return std::string{};
        }
        std::string text{content};
        // i18n `$key` 分支：首字符 `$` 视为 translations 查表 key。命中且
        // 翻译值非空 → 返回翻译文本；miss 或空翻译值 → fallback 到去 `$` 前
        // 缀的 key（对齐 MXU `translations?.[key] || key` 的 `||` 语义——
        // 空翻译值视为 falsy 也走 fallback）。空 key（content="$"）返空串
        // 是 MXU 行为，保留。
        if (text.front() == '$')
        {
            const std::string key{text.substr(1)};
            const auto        it = context.translations.find(key);
            if (it != context.translations.end() && !it->second.empty())
            {
                text = it->second;
            }
            else
            {
                return key;
            }
        }
        // URL 分支：http:// / https:// 前缀，经 BeastHttpClient 同步 GET（5s
        // 短超时）。成功 + status=200 → 返回 body（截断至 1 MiB 上限，SSRF
        // 内存耗尽防御）。失败 / 非 200 / http_client nullptr → 降级字面 URL
        // 文本，不阻塞 MaaFW 回调线程。
        if (text.starts_with("https://") || text.starts_with("http://"))
        {
            if (context.http_client != nullptr)
            {
                std::string body;
                int         status = 0;
                if (DAS::IsOk(context.http_client->Get(text, body, status))
                    && status == 200)
                {
                    if (body.size() > MAX_FOCUS_CONTENT_BYTES)
                    {
                        // 截断记 diagnostic warning，运维拿到截断后 release
                        // note 可区分源端残缺 vs 本地截断（与降级 ERROR 同
                        // spdlog 通路，落 logs/DasCore.log）。
                        const auto truncate_warning = DAS_FMT_NS::format(
                            "[focus-degraded] focus content body truncated "
                            "at 1 MiB (original_size={})",
                            body.size());
                        DAS_LOG_WARNING(truncate_warning.c_str());
                        body.resize(MAX_FOCUS_CONTENT_BYTES);
                    }
                    return body;
                }
            }
            return text;
        }
        // file 分支：./ 或 ../ 前缀，或含文档扩展名（.md/.txt/.html/.htm）
        // 启发式。从 context.interface_directory 拼接读文件；读失败/不存在
        // 降级字面文本。URL 优先于 file 避免 http://.../foo.md 被误判。
        const bool is_relative =
            text.starts_with("./") || text.starts_with("../");
        const bool has_doc_ext =
            text.size() > 4
            && (text.ends_with(".md") || text.ends_with(".txt")
                || text.ends_with(".html") || text.ends_with(".htm"));
        if (is_relative || has_doc_ext)
        {
            // 范围守卫：base/full 经 weakly_canonical 规范化后用 rfind(base,0)
            // ==0 比较前缀。绝对路径（path::operator/ 对绝对 RHS 丢弃 LHS）
            // 或 `../` 穿越只要越出 interface_directory 就读不到，降级字面
            // 文本。对齐 MXU 后端 read_local_file 限 exe 目录的范围语义。
            std::error_code ec;
            const auto      base = std::filesystem::weakly_canonical(
                context.interface_directory,
                ec);
            if (ec)
            {
                return text;
            }
            const auto full = std::filesystem::weakly_canonical(
                context.interface_directory / Das::Utils::PathFromUtf8(text),
                ec);
            if (ec || !std::filesystem::exists(full, ec))
            {
                return text;
            }
            // 字符串前缀判定有跨分量误匹配面（"dir_a" 前缀误中 "dir_ab"），
            // 成分迭代器逐分量比较才与目录包含语义一致。
            const auto prefix_mismatch = std::mismatch(
                base.begin(),
                base.end(),
                full.begin(),
                full.end());
            if (prefix_mismatch.first != base.end())
            {
                return text;
            }
            std::ifstream in{full, std::ios::binary};
            if (in.is_open())
            {
                std::string loaded(
                    (std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
                if (loaded.size() > MAX_FOCUS_CONTENT_BYTES)
                {
                    // 截断记 diagnostic warning（URL 分支同范式）。
                    const auto truncate_warning = DAS_FMT_NS::format(
                        "[focus-degraded] focus content body truncated "
                        "at 1 MiB (original_size={})",
                        loaded.size());
                    DAS_LOG_WARNING(truncate_warning.c_str());
                    loaded.resize(MAX_FOCUS_CONTENT_BYTES);
                }
                if (!loaded.empty())
                {
                    return loaded;
                }
            }
            return text;
        }
        // literal 兜底：未匹配任何外部源 → 原文返回（与既有行为兼容）。
        return text;
    }

    std::string FormatDisplayChannelsJson(
        const std::vector<std::string>& channels)
    {
        auto array = DAS::Utils::MakeYyjsonArray();
        auto array_ref = array.as_array();
        if (array_ref)
        {
            for (const auto& channel : channels)
            {
                array_ref->emplace_back(
                    std::string_view{channel},
                    yyjson::copy_string);
            }
        }
        auto json = DAS::Utils::SerializeYyjsonValue(array);
        return json ? *json : "[]";
    }

    void LogFocusMessage(
        MaaCallbackLoggerContext& context,
        const char*               message,
        const char*               details_json)
    {
        if (message == nullptr || details_json == nullptr || !context.recorder)
        {
            return;
        }
        auto parsed = DAS::Utils::ParseYyjsonFromString(details_json);
        if (!parsed)
        {
            return;
        }
        auto focus = ParseFocusTemplate(*parsed, message);
        if (!focus)
        {
            return;
        }
        // 先解析 content 模板（i18n `$key`/URL/file/literal 四分支）再走占位符
        // 替换——ResolveFocusContent 把 `$greeting` 展成翻译值（可能含
        // `{name}` 占位符），随后 ReplaceFocusPlaceholders 把翻译值中的
        // 占位符替换为 details 字段。两步组合保证 focus 模板支持
        // `$key.with.placeholder` 形式。
        focus->content = ResolveFocusContent(focus->content, context);
        std::string text = ReplaceFocusPlaceholders(focus->content, *parsed);
        // {image} 占位符：截图经 imageRefs 透传，文本中替换为空串，避免 UI
        // 渲染字面占位符（取不到截图 imageRefs 留空不阻塞 Record）。
        Das::DasPtr<Das::ExportInterface::IDasImage> image;
        if (text.find("{image}") != std::string::npos)
        {
            std::size_t pos = 0;
            while ((pos = text.find("{image}", pos)) != std::string::npos)
            {
                text.replace(pos, 7, "");
            }
            if (context.image_provider)
            {
                image = context.image_provider();
            }
        }
        const std::string display_channels =
            FormatDisplayChannelsJson(focus->channels);
        Das::DasPtr<Das::ExportInterface::IDasLogEntry> entry;
        if (!BuildFocusEntry(entry, text, image, display_channels))
        {
            return;
        }
        // recorder 失败降级：recorder 为空或 Record 返失败时按
        // DAS_LOG_TYPE_STRUCTURAL → DAS_LOG_INFO 打 [focus-degraded] 前缀
        // fallback，落 logs/DasCore.log（不再静默黑洞）。focus 全 STRUCTURAL
        // severity，与 BuildFocusEntry 既有 SetType(STRUCTURAL) 一致。
        (void)RecordEntryOrLog(
            context.recorder.Get(),
            entry.Get(),
            Das::ExportInterface::DAS_LOG_TYPE_STRUCTURAL,
            text);
    }

    DasResult RecordEntryOrLog(
        Das::ExportInterface::IDasProcessLogRecorder* recorder,
        Das::ExportInterface::IDasLogEntry*           entry,
        Das::ExportInterface::DasLogType              fallback_level,
        std::string_view                              fallback_text)
    {
        const DasResult rc =
            recorder ? recorder->Record(entry) : DAS_E_OBJECT_NOT_INIT;
        if (DAS::IsOk(rc))
        {
            return rc;
        }
        const std::string formatted =
            DAS_FMT_NS::format("[focus-degraded] {}", fallback_text);
        switch (fallback_level)
        {
        case Das::ExportInterface::DAS_LOG_TYPE_ERROR:
            DAS_LOG_ERROR(formatted.c_str());
            break;
        case Das::ExportInterface::DAS_LOG_TYPE_WARNING:
            DAS_LOG_WARNING(formatted.c_str());
            break;
        default:
            // STRUCTURAL / INFO / SUCCESS 全走 INFO（focus / agent 日志的
            // fallback 不区分 info 与 structural，统一 INFO 降级）。
            DAS_LOG_INFO(formatted.c_str());
            break;
        }
        return rc;
    }

    void DAS_MAA_CALL MaaCallbackLoggerEntry(
        void*       handle,
        const char* message,
        const char* details_json,
        void*       trans_arg)
    {
        (void)handle;
        if (message == nullptr || details_json == nullptr
            || trans_arg == nullptr)
        {
            return;
        }
        LogFocusMessage(
            *static_cast<MaaCallbackLoggerContext*>(trans_arg),
            message,
            details_json);
    }
} // namespace Das::Plugins::DasMaaPi

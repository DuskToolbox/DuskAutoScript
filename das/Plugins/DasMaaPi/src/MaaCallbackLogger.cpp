#include "MaaCallbackLogger.h"

#include <das/DasString.hpp>

#include <regex>
#include <utility>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
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
            if (DAS::IsFailed(
                    entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_FOCUS)))
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
                // {image} 由截图通路单独处理，此处保留原样（照搬 MXU）。
                result.append(match[0].first, match[0].second);
            }
            else if (details_obj && details_obj->contains(key))
            {
                // String(value) 语义（照搬 MXU）：字符串直接取，数字/bool
                // 转十进制文本；结构值（对象/数组）不替换，占位符原样保留
                // （T-88-11）。
                auto value = (*details_obj)[key];
                if (auto s = value.as_string())
                {
                    result.append(s->data(), s->size());
                }
                else if (auto num = value.as_sint())
                {
                    result.append(std::to_string(*num));
                }
                else if (auto num = value.as_uint())
                {
                    result.append(std::to_string(*num));
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
        std::string text = ReplaceFocusPlaceholders(focus->content, *parsed);
        // {image} 占位符：截图经 imageRefs 透传，文本中替换为空串，避免 UI
        // 渲染字面占位符（Pitfall 4：取不到截图 imageRefs 留空不阻塞 Record）。
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
        (void)context.recorder->Record(entry.Get());
    }
} // namespace Das::Plugins::DasMaaPi

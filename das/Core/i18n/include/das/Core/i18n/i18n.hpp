#ifndef DAS_CORE_I18N_I18N_H
#define DAS_CORE_I18N_I18N_H

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/Exceptions/TypeError.h>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/i18n/Config.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StreamUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>

class DasStringCppImpl;

DAS_CORE_I18N_NS_BEGIN

template <class T, class Item>
using TranslateItemMap = std::map<T, Item>;

template <class T, class Item>
using TranslateResources =
    std::unordered_map<std::u8string, TranslateItemMap<T, Item>>;

namespace Details
{
    template <class T>
    void CheckInput(const ExportInterface::DasType type)
    {
        if constexpr (std::is_same_v<T, std::int32_t>)
        {
            if (type != Das::ExportInterface::DAS_TYPE_INT)
            {
                throw DAS::Core::TypeError{
                    Das::ExportInterface::DAS_TYPE_INT,
                    type};
            }
        }
        else
        {
            static_assert(
                DAS::Utils::value<false, T>,
                "Incompatible type detected!");
        }
    }

    template <class T>
    constexpr auto GetConverter()
    {
        if constexpr (std::is_same_v<T, std::int32_t>)
        {
            return [](const std::string_view view)
            {
                char*      p_end{};
                const auto long_result = std::strtol(view.data(), &p_end, 0);
                const auto result = static_cast<std::int32_t>(long_result);
                if (long_result > std::numeric_limits<std::int32_t>::max()
                    || long_result < std::numeric_limits<std::int32_t>::min())
                {
                    DAS_CORE_LOG_WARN_USING_EXTRA_FUNCTION_NAME(
                        static_cast<const char*>(__FUNCTION__),
                        "Overflow detected: expected {}, std::int32_t value is {}.",
                        long_result,
                        result);
                }
                return result;
            };
        }
        else if constexpr (std::is_same_v<T, long long>)
        {
            return [](const std::string_view view)
            {
                char* p_end{};
                return std::strtoll(view.data(), &p_end, 0);
            };
        }
        else
        {
            static_assert(
                DAS::Utils::value<false, T>,
                "Incompatible type detected!");
        }
    }
} // namespace Details

template <class T>
class I18n
{
    using InternalTranslateResource =
        TranslateResources<T, DasReadOnlyStringWrapper>;
    using ConstLocaleToTranslateIt =
        typename InternalTranslateResource::const_iterator;

    InternalTranslateResource translate_resource_;
    ConstLocaleToTranslateIt  it_default_translate_map_;
    std::u8string             default_locale_;

    void ResetToEmptyState()
    {
        translate_resource_.clear();
        it_default_translate_map_ = translate_resource_.end();
        default_locale_.clear();
    }

public:
    explicit I18n(const std::filesystem::path& json_path)
    {
        ResetToEmptyState();
        std::ifstream ifs{};
        DAS::Utils::EnableStreamException(
            ifs,
            std::ios::badbit | std::ios::failbit,
            [&json_path](auto& stream) { stream.open(json_path); });
        std::string content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        auto parsed = Das::Utils::ParseYyjsonFromString(content);
        if (!parsed)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to parse i18n JSON: {}",
                DAS::Utils::U8AsString(json_path.u8string()));
            return;
        }
        ParseFromYyjsonValue(*parsed);
    }
    explicit I18n(const yyjson::value& json)
    {
        ResetToEmptyState();
        ParseFromYyjsonValue(json);
    }

    void ParseFromYyjsonValue(const yyjson::value& json)
    {
        ResetToEmptyState();

        auto obj = json.as_object();
        if (!obj)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to parse i18n: JSON root is not an object");
            return;
        }
        if (!obj->contains(std::string_view("type")))
        {
            DAS_CORE_LOG_ERROR(
                "Failed to parse i18n: missing or invalid 'type' field");
            return;
        }
        auto type_val = (*obj)[std::string_view("type")];
        auto type_opt = type_val.as_sint();
        if (!type_opt)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to parse i18n: missing or invalid 'type' field");
            return;
        }
        const auto type = static_cast<ExportInterface::DasType>(*type_opt);
        // NOTE: If T changes, we need to add code to handle this situation.
        try
        {
            Details::CheckInput<T>(type);
        }
        catch (const std::exception& ex)
        {
            DAS_CORE_LOG_ERROR("Failed to parse i18n: {}", ex.what());
            return;
        }
        const auto string_to_number_converter = Details::GetConverter<T>();

        if (!obj->contains(std::string_view("resource")))
        {
            DAS_CORE_LOG_ERROR(
                "Failed to parse i18n: missing or invalid 'resource' field");
            return;
        }
        auto resource_val = (*obj)[std::string_view("resource")];
        if (resource_val.is_null() || !resource_val.is_object())
        {
            DAS_CORE_LOG_ERROR(
                "Failed to parse i18n: missing or invalid 'resource' field");
            return;
        }
        InternalTranslateResource parsed_resource{};
        auto                      resource_obj = *resource_val.as_object();
        for (const auto& [locale_name, i18n_resource] : resource_obj)
        {
            TranslateItemMap<T, DasReadOnlyStringWrapper> tmp_map{};
            if (!i18n_resource.is_object())
            {
                continue;
            }
            auto resource_inner = *i18n_resource.as_object();
            for (const auto& [error_code_string, error_message] :
                 resource_inner)
            {
                T    error_code = string_to_number_converter(error_code_string);
                auto opt_str = error_message.as_string();
                if (opt_str)
                {
                    DasReadOnlyStringWrapper wrapper{std::string(*opt_str)};
                    tmp_map.emplace(error_code, std::move(wrapper));
                }
            }
            std::u8string locale_key(
                reinterpret_cast<const char8_t*>(locale_name.data()),
                locale_name.size());
            parsed_resource[std::move(locale_key)] = std::move(tmp_map);
        }
        translate_resource_ = std::move(parsed_resource);
        SetDefaultLocale(u8"en");
    }
    explicit I18n(const InternalTranslateResource& translate_resource)
        : translate_resource_{translate_resource}
    {
        SetDefaultLocale(u8"en");
    }

    explicit I18n(InternalTranslateResource&& translate_resource)
        : translate_resource_{std::move(translate_resource)}
    {
        SetDefaultLocale(u8"en");
    }

    ~I18n() = default;

    DasResult SetDefaultLocale(const char8_t* const default_locale)
    {
        default_locale_ = std::u8string(default_locale);
        if (const auto it = translate_resource_.find(default_locale_);
            it != translate_resource_.end())
        {
            it_default_translate_map_ = it;
            return DAS_S_OK;
        }
        it_default_translate_map_ = translate_resource_.end();
        return DAS_E_NO_IMPLEMENTATION;
    }
    std::u8string GetDefaultLocale() const { return default_locale_; }
    DasResult     GetErrorMessage(
        const T&             result,
        IDasReadOnlyString** pp_out_error_explanation) const
    {
        if (pp_out_error_explanation == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_error_explanation = nullptr;

        if (it_default_translate_map_ == translate_resource_.end())
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
        const auto& translate_table = it_default_translate_map_->second;
        if (const auto it = translate_table.find(result);
            it != translate_table.end())
        {
            it->second.GetImpl(pp_out_error_explanation);
            return DAS_S_OK;
        }
        return DAS_E_OUT_OF_RANGE;
    }
    DasResult GetErrorMessage(
        const char8_t* const locale,
        const T&             result,
        IDasReadOnlyString** pp_out_error_message) const
    {
        if (locale == nullptr || pp_out_error_message == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_error_message = nullptr;

        if (const auto resource_it = translate_resource_.find(locale);
            resource_it != translate_resource_.end())
        {
            const auto& table = resource_it->second;
            if (const auto it = table.find(result); it != table.end())
            {
                it->second.GetImpl(pp_out_error_message);
                return DAS_S_OK;
            }
            return DAS_E_OUT_OF_RANGE;
        }
        // fallback to default locale
        if (const auto en_us_resource_it = translate_resource_.find(u8"en");
            en_us_resource_it != translate_resource_.end())
        {
            const auto& table = en_us_resource_it->second;
            if (const auto it = table.find(result); it != table.end())
            {
                it->second.GetImpl(pp_out_error_message);
                return DAS_S_OK;
            }
        }
        return DAS_E_NO_IMPLEMENTATION;
    }
};

DAS_CORE_I18N_NS_END

#endif // DAS_CORE_I18N_I18N_H

#include <das/Core/i18n/IDasI18nImpl.h>

#include <das/Core/Logger/Logger.h>
#include <das/Core/i18n/GlobalLocale.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <das/Utils/StringUtils.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

DAS_CORE_I18N_NS_BEGIN

// ---------------------------------------------------------------------------
// DasI18nStringVectorImpl
// ---------------------------------------------------------------------------

DasResult DasI18nStringVectorImpl::GetCount(uint32_t* p_count)
{
    DAS_UTILS_CHECK_POINTER(p_count);

    *p_count = static_cast<uint32_t>(items_.size());
    return DAS_S_OK;
}

DasResult DasI18nStringVectorImpl::GetAt(
    uint32_t             index,
    IDasReadOnlyString** pp_out_value)
{
    DAS_UTILS_CHECK_POINTER(pp_out_value);

    if (index >= items_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    *pp_out_value = items_[index].Get();
    (*pp_out_value)->AddRef();
    return DAS_S_OK;
}

void DasI18nStringVectorImpl::AddString(const std::string& str)
{
    DAS::DasPtr<IDasReadOnlyString> p_str;
    if (DAS::IsOk(::CreateIDasReadOnlyStringFromUtf8(str.c_str(), p_str.Put()))
        && p_str.Get() != nullptr)
    {
        items_.push_back(std::move(p_str));
    }
}

void DasI18nStringVectorImpl::Reserve(size_t count) { items_.reserve(count); }

// ---------------------------------------------------------------------------
// IDasI18nImpl — 内部查表
// ---------------------------------------------------------------------------

DasResult IDasI18nImpl::Lookup(
    const std::string&   locale,
    const std::string&   key,
    IDasReadOnlyString** pp_out_text)
{
    // locale 大小写无关：统一小写查找（装载时 stem/入表已 toLower）。
    std::string normalized_locale = locale;
    DAS::Utils::ToLowerInPlace(normalized_locale);
    // 先查显式 locale
    if (const auto it = translations_.find(normalized_locale);
        it != translations_.end())
    {
        if (const auto kit = it->second.find(key); kit != it->second.end())
        {
            return ::CreateIDasReadOnlyStringFromUtf8(
                kit->second.c_str(),
                pp_out_text);
        }
    }
    // miss 且非 base 时回退 base locale（默认 en，已小写）
    if (normalized_locale != base_locale_)
    {
        if (const auto bit = translations_.find(base_locale_);
            bit != translations_.end())
        {
            if (const auto kit = bit->second.find(key);
                kit != bit->second.end())
            {
                return ::CreateIDasReadOnlyStringFromUtf8(
                    kit->second.c_str(),
                    pp_out_text);
            }
        }
    }
    return DAS_E_OUT_OF_RANGE;
}

// ---------------------------------------------------------------------------
// IDasI18n
// ---------------------------------------------------------------------------

DasResult IDasI18nImpl::Translate(
    IDasReadOnlyString*  key,
    IDasReadOnlyString** pp_out_text)
{
    DAS_UTILS_CHECK_POINTER(key);
    DAS_UTILS_CHECK_POINTER(pp_out_text);

    *pp_out_text = nullptr;
    const char* key_utf8 = nullptr;
    if (!DAS::IsOk(key->GetUtf8(&key_utf8)) || key_utf8 == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // 默认路径读进程级 GlobalLocale（DasGetDefaultLocale C API）
    DAS::DasPtr<IDasReadOnlyString> p_locale;
    if (!DAS::IsOk(::DasGetDefaultLocale(p_locale.Put()))
        || p_locale.Get() == nullptr)
    {
        return DAS_E_NO_IMPLEMENTATION;
    }
    const char* locale_utf8 = nullptr;
    p_locale->GetUtf8(&locale_utf8);
    const std::string locale_str =
        locale_utf8 != nullptr ? std::string(locale_utf8) : base_locale_;

    return Lookup(locale_str, std::string(key_utf8), pp_out_text);
}

DasResult IDasI18nImpl::TranslateWithLocale(
    IDasReadOnlyString*  locale_name,
    IDasReadOnlyString*  key,
    IDasReadOnlyString** pp_out_text)
{
    DAS_UTILS_CHECK_POINTER(locale_name);
    DAS_UTILS_CHECK_POINTER(key);
    DAS_UTILS_CHECK_POINTER(pp_out_text);

    *pp_out_text = nullptr;
    const char* locale_utf8 = nullptr;
    if (!DAS::IsOk(locale_name->GetUtf8(&locale_utf8))
        || locale_utf8 == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    const char* key_utf8 = nullptr;
    if (!DAS::IsOk(key->GetUtf8(&key_utf8)) || key_utf8 == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    return Lookup(std::string(locale_utf8), std::string(key_utf8), pp_out_text);
}

DasResult IDasI18nImpl::GetAvailableLocales(
    Das::ExportInterface::IDasReadOnlyStringVector** pp_out_locales)
{
    DAS_UTILS_CHECK_POINTER(pp_out_locales);

    *pp_out_locales = nullptr;
    try
    {
        auto* vec = new DasI18nStringVectorImpl();
        vec->AddRef();
        vec->Reserve(translations_.size());
        for (const auto& [locale, table] : translations_)
        {
            (void)table;
            vec->AddString(locale);
        }
        *pp_out_locales = vec;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult IDasI18nImpl::GetKeys(
    IDasReadOnlyString*                              locale_name,
    Das::ExportInterface::IDasReadOnlyStringVector** pp_out_keys)
{
    DAS_UTILS_CHECK_POINTER(locale_name);
    DAS_UTILS_CHECK_POINTER(pp_out_keys);

    *pp_out_keys = nullptr;
    const char* locale_utf8 = nullptr;
    if (!DAS::IsOk(locale_name->GetUtf8(&locale_utf8))
        || locale_utf8 == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // locale 大小写无关：统一小写查找（入表时已 toLower）。
    std::string normalized_locale(locale_utf8);
    DAS::Utils::ToLowerInPlace(normalized_locale);
    const auto it = translations_.find(normalized_locale);
    if (it == translations_.end())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    try
    {
        auto* vec = new DasI18nStringVectorImpl();
        vec->AddRef();
        vec->Reserve(it->second.size());
        for (const auto& [k, text] : it->second)
        {
            (void)text;
            vec->AddString(k);
        }
        *pp_out_keys = vec;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ---------------------------------------------------------------------------
// IDasMutableI18n
// ---------------------------------------------------------------------------

DasResult IDasI18nImpl::AddTranslation(
    IDasReadOnlyString* locale_name,
    IDasReadOnlyString* key,
    IDasReadOnlyString* text)
{
    DAS_UTILS_CHECK_POINTER(locale_name);
    DAS_UTILS_CHECK_POINTER(key);
    DAS_UTILS_CHECK_POINTER(text);

    const char* locale_utf8 = nullptr;
    if (!DAS::IsOk(locale_name->GetUtf8(&locale_utf8))
        || locale_utf8 == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    const char* key_utf8 = nullptr;
    if (!DAS::IsOk(key->GetUtf8(&key_utf8)) || key_utf8 == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    const char* text_utf8 = nullptr;
    if (!DAS::IsOk(text->GetUtf8(&text_utf8)) || text_utf8 == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    std::string locale_str(locale_utf8);
    DAS::Utils::ToLowerInPlace(locale_str); // locale 大小写无关：统一小写入表
    const std::string key_str(key_utf8);
    const std::string text_str(text_utf8);
    if (locale_str.empty() || key_str.empty())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    try
    {
        translations_[locale_str][key_str] = text_str; // 覆盖语义
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    return DAS_S_OK;
}

DasResult IDasI18nImpl::MergeFromFile(IDasReadOnlyString* json_path_utf8)
{
    DAS_UTILS_CHECK_POINTER(json_path_utf8);

    const char* path_utf8 = nullptr;
    if (!DAS::IsOk(json_path_utf8->GetUtf8(&path_utf8)) || path_utf8 == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    return MergeSingleFile(Das::Utils::PathFromUtf8(path_utf8));
}

// ---------------------------------------------------------------------------
// 装载入口
// ---------------------------------------------------------------------------

DasResult IDasI18nImpl::LoadDirectory(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const auto& path = entry.path();
        if (path.extension() != DAS_PATH_LITERAL(".json"))
        {
            continue;
        }
        const auto merge_result = MergeSingleFile(path);
        if (DAS::IsFailed(merge_result))
        {
            // 单文件畸形中止整个装载（事务性：已合并的 locale 保留，但 factory
            // 返回失败让调用方知道资源有问题）。
            return merge_result;
        }
    }
    return DAS_S_OK;
}

DasResult IDasI18nImpl::MergeSingleFile(const std::filesystem::path& json_path)
{
    std::ifstream ifs(json_path, std::ios::binary);
    if (!ifs.is_open())
    {
        DAS_CORE_LOG_ERROR(
            "i18n merge: cannot open json file: {}",
            Das::Utils::PathToUtf8(json_path));
        return DAS_E_INVALID_JSON;
    }
    const std::string content(
        (std::istreambuf_iterator<char>(ifs)),
        std::istreambuf_iterator<char>());

    const auto parsed = DAS::Utils::ParseYyjsonFromString(content);
    if (!parsed)
    {
        DAS_CORE_LOG_ERROR(
            "i18n merge: invalid JSON in file: {}",
            Das::Utils::PathToUtf8(json_path));
        return DAS_E_INVALID_JSON;
    }
    const auto obj = parsed->as_object();
    if (!obj)
    {
        DAS_CORE_LOG_ERROR(
            "i18n merge: JSON root is not an object in file: {}",
            Das::Utils::PathToUtf8(json_path));
        return DAS_E_INVALID_JSON;
    }

    std::string locale = Das::Utils::PathToUtf8(json_path.stem());
    // locale 大小写无关：文件名 stem 统一小写入表（与 Lookup 的 normalized
    // 查询对齐）。
    DAS::Utils::ToLowerInPlace(locale);

    // 事务性：先解析进临时 map，全成功再合并入 translations_，畸形不污染既有表
    KeyToText tmp;
    for (const auto& [json_key, value] : *obj)
    {
        const auto str_opt = value.as_string();
        if (!str_opt)
        {
            DAS_CORE_LOG_ERROR(
                "i18n merge: value for key '{}' is not a string in file: {}",
                std::string(json_key),
                Das::Utils::PathToUtf8(json_path));
            return DAS_E_INVALID_JSON;
        }
        tmp[std::string(json_key)] = std::string(*str_opt);
    }

    auto& target = translations_[locale];
    for (auto& [k, v] : tmp)
    {
        target[k] = std::move(v); // 覆盖语义：已存在 key 被新值覆盖
    }
    return DAS_S_OK;
}

DAS_CORE_I18N_NS_END

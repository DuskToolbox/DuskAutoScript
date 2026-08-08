#include <das/Core/i18n/IDasI18nImpl.h>

#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/i18n/GlobalLocale.h>
#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp>

DAS_CORE_I18N_NS_BEGIN

DasResult DasI18nStringVectorImpl::GetCount(uint32_t* p_count)
{
    DAS_UTILS_CHECK_POINTER(p_count);

    *p_count = 0;
    return DAS_S_OK;
}

DasResult DasI18nStringVectorImpl::GetAt(
    uint32_t             index,
    IDasReadOnlyString** pp_out_value)
{
    DAS_UTILS_CHECK_POINTER(pp_out_value);

    (void)index;
    return DAS_E_OUT_OF_RANGE;
}

void DasI18nStringVectorImpl::AddString(const std::string& str) { (void)str; }

void DasI18nStringVectorImpl::Reserve(size_t count) { (void)count; }

DasResult IDasI18nImpl::Translate(
    IDasReadOnlyString*  key,
    IDasReadOnlyString** pp_out_text)
{
    (void)key;
    (void)pp_out_text;
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult IDasI18nImpl::TranslateWithLocale(
    IDasReadOnlyString*  locale_name,
    IDasReadOnlyString*  key,
    IDasReadOnlyString** pp_out_text)
{
    (void)locale_name;
    (void)key;
    (void)pp_out_text;
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult IDasI18nImpl::GetAvailableLocales(
    Das::ExportInterface::IDasReadOnlyStringVector** pp_out_locales)
{
    DAS_UTILS_CHECK_POINTER(pp_out_locales);

    *pp_out_locales = nullptr;
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult IDasI18nImpl::GetKeys(
    IDasReadOnlyString*                              locale_name,
    Das::ExportInterface::IDasReadOnlyStringVector** pp_out_keys)
{
    (void)locale_name;
    DAS_UTILS_CHECK_POINTER(pp_out_keys);

    *pp_out_keys = nullptr;
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult IDasI18nImpl::AddTranslation(
    IDasReadOnlyString* locale_name,
    IDasReadOnlyString* key,
    IDasReadOnlyString* text)
{
    (void)locale_name;
    (void)key;
    (void)text;
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult IDasI18nImpl::MergeFromFile(IDasReadOnlyString* json_path_utf8)
{
    (void)json_path_utf8;
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult IDasI18nImpl::LoadDirectory(const std::filesystem::path& dir)
{
    (void)dir;
    return DAS_S_OK;
}

DasResult IDasI18nImpl::Lookup(
    const std::string&   locale,
    const std::string&   key,
    IDasReadOnlyString** pp_out_text)
{
    (void)locale;
    (void)key;
    (void)pp_out_text;
    return DAS_E_OUT_OF_RANGE;
}

DasResult IDasI18nImpl::MergeSingleFile(const std::filesystem::path& json_path)
{
    (void)json_path;
    return DAS_E_NO_IMPLEMENTATION;
}

DAS_CORE_I18N_NS_END

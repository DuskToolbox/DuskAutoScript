#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/i18n/GlobalLocale.h>
#include <das/PluginInterface/IDasErrorLens.h>

DAS_CORE_I18N_NS_BEGIN

GlobalLocaleSingleton InitializeGlobalLocaleSingleton()
{
    return GlobalLocaleSingleton{};
}

GlobalLocaleSingleton::GlobalLocaleSingleton()
{
    ::CreateIDasReadOnlyStringFromUtf8("en", p_locale_name.Put());
}

const Das::DasPtr<IDasReadOnlyString>& GlobalLocaleSingleton::GetInstance()
    const
{
    return p_locale_name;
}

void GlobalLocaleSingleton::SetInstance(IDasReadOnlyString& p_new_locale_name)
{
    p_locale_name = &p_new_locale_name;
}

DAS_DEFINE_VARIABLE(g_locale) = InitializeGlobalLocaleSingleton();

const auto g_fallback_locale_name{
    MakeDasPtr<IDasReadOnlyString, ::DasStringCppImpl>(
        U_NAMESPACE_QUALIFIER UnicodeString::fromUTF8("en"))};

auto GetFallbackLocale() -> DasPtr<IDasReadOnlyString>
{
    return g_fallback_locale_name;
}

DAS_CORE_I18N_NS_END

// ----------------------------------------------------------------

DasResult DasSetDefaultLocale(IDasReadOnlyString* locale_name)
{
    DAS_UTILS_CHECK_POINTER(locale_name)

    DAS::Core::i18n::g_locale.SetInstance(*locale_name);
    return DAS_S_OK;
}

DasResult DasGetDefaultLocale(IDasReadOnlyString** pp_out_locale_name)
{
    DAS_UTILS_CHECK_POINTER(pp_out_locale_name)

    auto* p_result = DAS::Core::i18n::g_locale.GetInstance().Get();
    p_result->AddRef();
    *pp_out_locale_name = p_result;
    return DAS_S_OK;
}

DasResult DasSetDefaultLocale(DasReadOnlyString locale_name)
{
    DAS::DasPtr<IDasReadOnlyString> p_locale_name{};
    locale_name.GetImpl(p_locale_name.Put());
    return ::DasSetDefaultLocale(p_locale_name.Get());
}

DasRetReadOnlyString DasGetDefaultLocale()
{
    DasRetReadOnlyString            result{};
    DAS::DasPtr<IDasReadOnlyString> p_locale_name{};
    result.error_code = ::DasGetDefaultLocale(p_locale_name.Put());
    result.value = DasReadOnlyString{p_locale_name};
    return result;
}

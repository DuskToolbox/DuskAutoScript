#include <das/Core/i18n/DasLocaleServiceImpl.h>

DAS_CORE_I18N_NS_BEGIN

DAS_IMPL DasLocaleServiceImpl::AddCallback(
    Das::ExportInterface::IDasLocaleCallback* callback)
{
    (void)callback;
    return DAS_E_FAIL;
}

DAS_IMPL DasLocaleServiceImpl::RemoveCallback(
    Das::ExportInterface::IDasLocaleCallback* callback)
{
    (void)callback;
    return DAS_E_FAIL;
}

DAS_IMPL DasLocaleServiceImpl::GetDefaultLocale(
    IDasReadOnlyString** pp_out_locale)
{
    (void)pp_out_locale;
    return DAS_E_FAIL;
}

DAS_IMPL DasLocaleServiceImpl::SetDefaultLocale(IDasReadOnlyString* locale)
{
    (void)locale;
    return DAS_E_FAIL;
}

DAS_CORE_I18N_NS_END

#ifndef DAS_CORE_I18N_GLOBALLOCALE_H
#define DAS_CORE_I18N_GLOBALLOCALE_H

#include <das/Core/i18n/Config.h>
#include <das/DasString.hpp>

DAS_CORE_I18N_NS_BEGIN

class GlobalLocaleSingleton
{
    friend GlobalLocaleSingleton InitializeGlobalLocaleSingleton();

    DasPtr<IDasReadOnlyString> p_locale_name;

    GlobalLocaleSingleton();

public:
    const DasPtr<IDasReadOnlyString>& GetInstance() const;
    void SetInstance(IDasReadOnlyString& p_new_locale_name);
};

extern GlobalLocaleSingleton g_locale;

auto GetFallbackLocale() -> DasPtr<IDasReadOnlyString>;

DAS_CORE_I18N_NS_END

#endif // DAS_CORE_I18N_GLOBALLOCALE_H

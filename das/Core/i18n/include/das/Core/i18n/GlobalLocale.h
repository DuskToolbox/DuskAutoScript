#ifndef DAS_CORE_I18N_GLOBALLOCALE_H
#define DAS_CORE_I18N_GLOBALLOCALE_H

#include <das/Core/i18n/Config.h>
#include <das/DasString.hpp>

DAS_CORE_I18N_NS_BEGIN

class GlobalLocaleSingleton
{
    DasPtr<IDasReadOnlyString> p_locale_name_;

    GlobalLocaleSingleton();

public:
    GlobalLocaleSingleton(const GlobalLocaleSingleton&) = delete;
    GlobalLocaleSingleton& operator=(const GlobalLocaleSingleton&) = delete;
    GlobalLocaleSingleton(GlobalLocaleSingleton&&) = delete;
    GlobalLocaleSingleton& operator=(GlobalLocaleSingleton&&) = delete;

    static GlobalLocaleSingleton& GetInstance();
    void SetLocale(IDasReadOnlyString* p_new_locale_name);
    const DasPtr<IDasReadOnlyString>& GetLocale();
};

auto GetFallbackLocale() -> DasPtr<IDasReadOnlyString>;

DAS_CORE_I18N_NS_END

#endif // DAS_CORE_I18N_GLOBALLOCALE_H

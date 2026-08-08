#ifndef DAS_CORE_I18N_GLOBALLOCALE_H
#define DAS_CORE_I18N_GLOBALLOCALE_H

#include <das/Core/i18n/Config.h>
#include <das/DasString.hpp>
#include <mutex>

DAS_CORE_I18N_NS_BEGIN

class GlobalLocaleSingleton
{
    friend GlobalLocaleSingleton InitializeGlobalLocaleSingleton();

    mutable std::mutex         mutex_;
    DasPtr<IDasReadOnlyString> p_locale_name;

    GlobalLocaleSingleton();

public:
    // 锁内拷贝(AddRef)返回独立副本，不再返回内部引用：并发 Get/Set 下
    // 避免「取指针→AddRef」窗口内对象被 Set 释放致 UAF；Get/Set
    // 各成原子临界区。
    DasPtr<IDasReadOnlyString> GetInstance() const;
    void SetInstance(IDasReadOnlyString& p_new_locale_name);
};

extern GlobalLocaleSingleton g_locale;

auto GetFallbackLocale() -> DasPtr<IDasReadOnlyString>;

DAS_CORE_I18N_NS_END

#endif // DAS_CORE_I18N_GLOBALLOCALE_H

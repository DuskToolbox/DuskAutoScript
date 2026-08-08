#include "DasLocaleCallbackImpl.h"

#include <das/DasApi.h>

DAS_CORE_I18N_NS_BEGIN

DasResult DasLocaleCallbackImpl::OnDefaultLocaleChanged(
    IDasReadOnlyString* new_locale)
{
    if (new_locale == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    // 主进程切语言 → 经 IPC 反向通知 → 同步本地 GlobalLocale（mutex
    // 保护，线程安全）
    return ::DasSetDefaultLocale(new_locale);
}

DAS_CORE_I18N_NS_END

// C ABI factory（IDL [export,c_abi] 声明，Core 内 new 返回接口指针）。
// Host（DasHost main.cpp）调本 factory 创建反向监听 callback，经 DasPtr 管理。
DasResult CreateIDasLocaleCallback(
    Das::ExportInterface::IDasLocaleCallback** pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out = nullptr;
    auto* impl = new Das::Core::i18n::DasLocaleCallbackImpl();
    impl->AddRef();
    *pp_out = impl;
    return DAS_S_OK;
}

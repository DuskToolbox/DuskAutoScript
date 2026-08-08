#ifndef DAS_CORE_I18N_SRC_DASLOCALECALLBACKIMPL_H
#define DAS_CORE_I18N_SRC_DASLOCALECALLBACKIMPL_H

#include <das/Core/i18n/Config.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasLocaleService.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasLocaleCallback.Implements.hpp>

DAS_CORE_I18N_NS_BEGIN

// 反向通知 callback（IDasLocaleCallback，无状态 forwarder）。
// 主进程切语言时经 service proxy 跨进程回调 OnDefaultLocaleChanged，
// 直接 ::DasSetDefaultLocale 同步本地 GlobalLocale（mutex 线程安全）。
// 无状态——不持有任何 Owner，生命周期由调用方（DasHost main.cpp）DasPtr 管理。
class DasLocaleCallbackImpl final
    : public Das::ExportInterface::DasLocaleCallbackImplBase<
          DasLocaleCallbackImpl>
{
public:
    DasResult OnDefaultLocaleChanged(IDasReadOnlyString* new_locale) override;
};

DAS_CORE_I18N_NS_END

#endif // DAS_CORE_I18N_SRC_DASLOCALECALLBACKIMPL_H

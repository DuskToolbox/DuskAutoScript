#ifndef DAS_CORE_I18N_SRC_DASLOCALECALLBACKIMPL_H
#define DAS_CORE_I18N_SRC_DASLOCALECALLBACKIMPL_H

#include <das/Core/i18n/Config.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasLocaleService.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasLocaleCallback.Implements.hpp>

#include <memory>

DAS_CORE_I18N_NS_BEGIN

// RAII owner 持 service proxy + 注册到 service 的反向通知 callback。
//
// CONTEXT 决策：locale listener 是进程级单例（跟进程同寿），断连泄漏随进程
// 退出整体回收不累积 → 可忽略。LocaleSyncCallback::Do() 把本对象挂在
// 进程级 static shared_ptr 上保活（首次触发构造，后续触发空转）。
//
// 析构 RAII RemoveCallback 兜底（防悬挂注册）。weak_ptr<Owner> 兜底：
// 若 Owner 已析构（理论上进程退出阶段），DasLocaleCallbackImpl 的
// OnDefaultLocaleChanged 经 lock() 判空早返回，不 UAF（Pattern 3，照
// DasProgressReporter.cpp:197-315 范式）。
struct LocaleSyncOwner final
    : public std::enable_shared_from_this<LocaleSyncOwner>
{
    Das::DasPtr<Das::ExportInterface::IDasLocaleService>  service;
    Das::DasPtr<Das::ExportInterface::IDasLocaleCallback> callback;
    bool                                                  started = false;

    ~LocaleSyncOwner();
};

// 反向通知 callback（IDasLocaleCallback 本地实现）。主进程切语言时经
// service proxy 跨进程回调 OnDefaultLocaleChanged。持 weak_ptr<Owner>
// （不增引用计数），若 Owner 已析构 lock() 失败空转防 UAF。
class DasLocaleCallbackImpl final
    : public Das::ExportInterface::DasLocaleCallbackImplBase<
          DasLocaleCallbackImpl>
{
public:
    explicit DasLocaleCallbackImpl(std::weak_ptr<LocaleSyncOwner> owner)
        : owner_(std::move(owner))
    {
    }

    DasResult OnDefaultLocaleChanged(IDasReadOnlyString* new_locale) override;

private:
    std::weak_ptr<LocaleSyncOwner> owner_;
};

DAS_CORE_I18N_NS_END

#endif // DAS_CORE_I18N_SRC_DASLOCALECALLBACKIMPL_H

#include "DasLocaleCallbackImpl.h"

#include <das/DasApi.h>
#include <das/Utils/fmt.h>

DAS_CORE_I18N_NS_BEGIN

LocaleSyncOwner::~LocaleSyncOwner()
{
    if (started && service && callback)
    {
        // RAII 兜底：RemoveCallback 触发 DasPtr 析构 Release → 远端释放链。
        // 失败仅记日志（断连场景 best-effort，Pitfall 3）。
        const auto rc = service->RemoveCallback(callback.Get());
        if (DAS::IsFailed(rc))
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "LocaleSyncOwner: RemoveCallback failed result={}",
                    static_cast<int>(rc))
                    .c_str());
        }
    }
}

DasResult DasLocaleCallbackImpl::OnDefaultLocaleChanged(
    IDasReadOnlyString* new_locale)
{
    if (new_locale == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    // weak_ptr 兜底：Owner 已析构（理论上仅进程退出阶段）时空转不 UAF。
    const std::shared_ptr<LocaleSyncOwner> locked = owner_.lock();
    if (!locked)
    {
        return DAS_S_OK;
    }
    // 主进程切语言 → 经 IPC 反向通知到本 Host 进程 → 同步本地 GlobalLocale。
    return ::DasSetDefaultLocale(new_locale);
}

DAS_CORE_I18N_NS_END

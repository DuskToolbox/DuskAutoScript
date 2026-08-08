#include <das/Core/i18n/DasLocaleServiceImpl.h>

#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Utils/fmt.h>

#include <algorithm>
#include <utility>
#include <vector>

DAS_CORE_I18N_NS_BEGIN

DAS_IMPL DasLocaleServiceImpl::AddCallback(
    Das::ExportInterface::IDasLocaleCallback* callback)
{
    if (!callback)
    {
        return DAS_E_INVALID_POINTER;
    }
    const std::unique_lock lock(mutex_);
    // DasPtr(callback) 构造 AddRef，service 持有独立引用；erase 时 Release。
    callbacks_.emplace_back(callback);
    return DAS_S_OK;
}

DAS_IMPL DasLocaleServiceImpl::RemoveCallback(
    Das::ExportInterface::IDasLocaleCallback* callback)
{
    if (!callback)
    {
        return DAS_E_INVALID_POINTER;
    }
    const std::unique_lock lock(mutex_);
    const auto             it = std::find_if(
        callbacks_.begin(),
        callbacks_.end(),
        [callback](
            const Das::DasPtr<Das::ExportInterface::IDasLocaleCallback>& stored)
        { return stored.Get() == callback; });
    if (it == callbacks_.end())
    {
        return DAS_E_NOT_FOUND;
    }
    // DasPtr 析构 Release → 触发远端释放链（防 DOM 泄漏）。
    callbacks_.erase(it);
    return DAS_S_OK;
}

DAS_IMPL DasLocaleServiceImpl::GetDefaultLocale(
    IDasReadOnlyString** pp_out_locale)
{
    if (!pp_out_locale)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_locale = nullptr;
    // 直接委托进程级 GlobalLocale C API（默认 "en"）。
    return ::DasGetDefaultLocale(pp_out_locale);
}

DAS_IMPL DasLocaleServiceImpl::SetDefaultLocale(IDasReadOnlyString* locale)
{
    if (!locale)
    {
        return DAS_E_INVALID_POINTER;
    }

    // ① 更新进程级 GlobalLocale（即时生效，IDasI18n.Translate 默认路径读它）。
    const auto set_result = ::DasSetDefaultLocale(locale);
    if (DAS::IsFailed(set_result))
    {
        return set_result;
    }

    // ② 反向通知所有已注册 callback：持锁拷贝 callbacks_ 副本后锁外遍历，
    // 防 callback 内回调 service 致重入死锁（照 DasProgressServiceImpl
    // :218-250 范式）。
    std::vector<Das::DasPtr<Das::ExportInterface::IDasLocaleCallback>> snapshot;
    {
        const std::unique_lock lock(mutex_);
        snapshot = callbacks_;
    }

    for (const auto& callback : snapshot)
    {
        const auto rc = callback->OnDefaultLocaleChanged(locale);
        if (DAS::IsFailed(rc))
        {
            DAS_CORE_LOG_ERROR(
                "DasLocaleServiceImpl: OnDefaultLocaleChanged failed result={}",
                rc);
        }
    }
    return DAS_S_OK;
}

DAS_CORE_I18N_NS_END

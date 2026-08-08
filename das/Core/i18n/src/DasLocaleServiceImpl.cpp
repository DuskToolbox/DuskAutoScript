#include "DasLocaleServiceImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>

#include <algorithm>
#include <utility>

DAS_CORE_I18N_NS_BEGIN

DAS_IMPL DasLocaleServiceImpl::AddCallback(
    Das::ExportInterface::IDasLocaleCallback* callback)
{
    if (!callback)
    {
        return DAS_E_INVALID_POINTER;
    }
    const std::unique_lock lock(mutex_);
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
    // 直接委托进程级 GlobalLocale C API（默认 "en"，mutex 保护线程安全）。
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
    // 防 callback 内回调 service 致重入死锁。
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
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "DasLocaleServiceImpl: OnDefaultLocaleChanged failed "
                    "result={}",
                    static_cast<int>(rc))
                    .c_str());
        }
    }
    return DAS_S_OK;
}

DAS_CORE_I18N_NS_END

// C ABI factory（IDL [export,c_abi] 声明，Core 内 new 返回接口指针）。
// DasHttp 调本 factory 创建服务实例，不直接 new Core 类、不 include 内部
// header。
DasResult CreateDasLocaleServiceImpl(
    Das::ExportInterface::IDasLocaleService** pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out = nullptr;
    auto* impl = new Das::Core::i18n::DasLocaleServiceImpl();
    impl->AddRef();
    *pp_out = impl;
    return DAS_S_OK;
}

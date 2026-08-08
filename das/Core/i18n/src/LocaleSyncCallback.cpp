#include "DasLocaleCallbackImpl.h"

#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/i18n/LocaleSyncCallback.h>
#include <das/DasApi.h>
#include <das/IDasAsyncCallback.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasLocaleService.h>

#include <atomic>

DAS_CORE_I18N_NS_BEGIN

// on_after_connect 经 PostToBusinessThread 投递的异步回调。Do() 在 BT 上跑
// locale 拉取（握手已结束，transport 空闲）。无状态一次性任务——Do() 跑完
// 即弃（main 不长期持有 cb）。
class LocaleSyncCallback final : public IDasAsyncCallback
{
public:
    DasResult Do() override;

    uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

    uint32_t DAS_STD_CALL Release() override
    {
        const auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out_object) override
    {
        if (pp_out_object == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_object = nullptr;
        if (iid == DasIidOf<IDasBase>() || iid == DasIidOf<IDasAsyncCallback>())
        {
            AddRef();
            *pp_out_object = static_cast<IDasAsyncCallback*>(this);
            return DAS_S_OK;
        }
        return DAS_E_NO_INTERFACE;
    }

private:
    std::atomic<uint32_t> ref_count_{0};
};

DasResult LocaleSyncCallback::Do()
{
    auto* ctx = DAS::Core::IPC::GetCurrentIpcContext();
    if (ctx == nullptr)
    {
        DAS_LOG_ERROR(
            "LocaleSyncCallback: no current IPC context (must run on BT)");
        return DAS_E_FAIL;
    }

    // resolve das.locale（BT 上 sync_wait + PumpUntilResponse，安全）
    Das::DasPtr<IDasBase>                                raw;
    Das::DasPtr<Das::ExportInterface::IDasLocaleService> service;
    if (Das::IsFailed(
            ctx->ResolveMainProcessInterfaceByName("das.locale", raw.Put()))
        || !raw)
    {
        DAS_LOG_ERROR("LocaleSyncCallback: resolve das.locale failed");
        return DAS_E_FAIL;
    }
    if (Das::IsFailed(raw.As(service.Put())) || !service)
    {
        DAS_LOG_ERROR("LocaleSyncCallback: QI IDasLocaleService failed");
        return DAS_E_FAIL;
    }

    // 创建反向监听 callback（无状态 forwarder）并注册到主进程。
    // callback 靠主进程跨进程引用保活（断连 best-effort 释放）。
    Das::DasPtr<Das::ExportInterface::IDasLocaleCallback> callback;
    if (Das::IsFailed(::CreateIDasLocaleCallback(callback.Put())) || !callback)
    {
        DAS_LOG_ERROR("LocaleSyncCallback: CreateIDasLocaleCallback failed");
        return DAS_E_FAIL;
    }
    if (const auto rc = service->AddCallback(callback.Get()); Das::IsFailed(rc))
    {
        DAS_LOG_ERROR(
            DAS_FMT_NS::format(
                "LocaleSyncCallback: AddCallback failed result={}",
                static_cast<int>(rc))
                .c_str());
        return rc;
    }

    // 拉主进程当前 locale 到本进程 GlobalLocale
    Das::DasPtr<IDasReadOnlyString> locale;
    if (Das::IsFailed(service->GetDefaultLocale(locale.Put())) || !locale)
    {
        DAS_LOG_ERROR("LocaleSyncCallback: GetDefaultLocale failed");
        return DAS_E_FAIL;
    }
    if (const auto rc = ::DasSetDefaultLocale(locale.Get()); Das::IsFailed(rc))
    {
        DAS_LOG_ERROR(
            DAS_FMT_NS::format(
                "LocaleSyncCallback: DasSetDefaultLocale failed result={}",
                static_cast<int>(rc))
                .c_str());
        return rc;
    }
    return DAS_S_OK;
}

DAS_CORE_I18N_NS_END

// C ABI factory（out-param，调用方 DasPtr 接收，配对无泄漏）。
DAS_C_API void CreateLocaleSyncCallback(IDasAsyncCallback** pp_out)
{
    if (pp_out == nullptr)
    {
        return;
    }
    *pp_out = nullptr;
    auto* p = new Das::Core::i18n::LocaleSyncCallback();
    p->AddRef(); // ref_count_ 0→1，给调用方
    *pp_out = p;
}

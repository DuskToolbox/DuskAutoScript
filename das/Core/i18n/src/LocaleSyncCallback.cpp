#include "DasLocaleCallbackImpl.h"

#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/DasApi.h>
#include <das/IDasAsyncCallback.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasLocaleService.h>

#include <atomic>
#include <memory>
#include <utility>

DAS_CORE_I18N_NS_BEGIN

// IDasAsyncCallback 实现：在 BusinessThread 上跑 locale 同步序列。每次
// on_after_connect 触发时由 main.cpp 的 lambda 经 PostToBusinessThread
// 投递本对象（fire-and-forget，InternalCallbackHandler 接收端调 Do()）。
//
// 单次初始化：首次 Do() 构造 LocaleSyncOwner 进程级单例并跑完整序列
// （resolve + AddCallback + GetDefaultLocale + SetDefaultLocale），后续
// 触发空转。CONTEXT：locale listener 跟进程同寿，断连泄漏不累积。
//
// 类本体不暴露在任何 public header——消费方（DasHostX.exe）只经
// MakeLocaleSyncCallback() C 工厂拿抽象 IDasAsyncCallback*，不引用类
// vtable，跨 DLL 安全（照 DasLocaleServiceImpl.h 头文件范式的精神）。
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

namespace
{
    // 进程级 LocaleSyncOwner 单例（C++11 magic statics 保证线程安全首次
    // 初始化）。断连/重连场景下 on_after_connect 可能多次触发，static
    // 保证只构造一次；后续 Do() 见 started=true 早返回。
    std::shared_ptr<LocaleSyncOwner> EnsureLocaleSyncOwner()
    {
        static std::shared_ptr<LocaleSyncOwner> owner = []
        { return std::make_shared<LocaleSyncOwner>(); }();
        return owner;
    }

    // 在 BusinessThread 上跑 locale 同步序列（resolve 走 sync_wait +
    // PumpUntilResponse，BT 上安全；on_after_connect lambda 跑 IO 线程
    // 绝不能调本函数，Pitfall 1）。
    DasResult InitializeLocaleSyncOwner(
        const std::shared_ptr<LocaleSyncOwner>& owner)
    {
        auto* ctx = DAS::Core::IPC::GetCurrentIpcContext();
        if (ctx == nullptr)
        {
            DAS_LOG_ERROR(
                "LocaleSyncCallback: no current IPC context (must run on "
                "BusinessThread)");
            return DAS_E_FAIL;
        }

        Das::DasPtr<IDasBase> raw;
        const auto            resolve_rc =
            ctx->ResolveMainProcessInterfaceByName("das.locale", raw.Put());
        if (DAS::IsFailed(resolve_rc) || !raw)
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "LocaleSyncCallback: resolve das.locale failed result={}",
                    static_cast<int>(resolve_rc))
                    .c_str());
            return resolve_rc;
        }
        const auto qi_rc = raw.As(owner->service.Put());
        if (DAS::IsFailed(qi_rc) || !owner->service)
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "LocaleSyncCallback: QI IDasLocaleService failed result={}",
                    static_cast<int>(qi_rc))
                    .c_str());
            return qi_rc;
        }

        // 构造 callback 持 weak_ptr<Owner>（不增 Owner 引用计数，防
        // shared_ptr 循环）。AddCallback 经 IDL autogen 自动代理化 callback
        // 参数（SerializeInInterfaceParam），跨进程注册到主进程
        // DasLocaleServiceImpl.callbacks_。
        owner->callback = DasLocaleCallbackImpl::Make(owner->weak_from_this());
        if (!owner->callback)
        {
            DAS_LOG_ERROR(
                "LocaleSyncCallback: DasLocaleCallbackImpl::Make returned null");
            return DAS_E_FAIL;
        }
        const auto add_rc = owner->service->AddCallback(owner->callback.Get());
        if (DAS::IsFailed(add_rc))
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "LocaleSyncCallback: AddCallback failed result={}",
                    static_cast<int>(add_rc))
                    .c_str());
            return add_rc;
        }

        // pull 主进程当前 locale → 应用到本 Host 进程 GlobalLocale。失败不
        // 致命：callback 已注册，主进程后续切语言仍经反向通知补回。
        Das::DasPtr<IDasReadOnlyString> locale;
        const auto get_rc = owner->service->GetDefaultLocale(locale.Put());
        if (DAS::IsOk(get_rc) && locale)
        {
            const auto set_rc = ::DasSetDefaultLocale(locale.Get());
            if (DAS::IsFailed(set_rc))
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "LocaleSyncCallback: DasSetDefaultLocale failed "
                        "result={}",
                        static_cast<int>(set_rc))
                        .c_str());
            }
        }
        else
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "LocaleSyncCallback: GetDefaultLocale failed result={}",
                    static_cast<int>(get_rc))
                    .c_str());
        }

        owner->started = true;
        DAS_LOG_INFO("LocaleSyncCallback: locale sync initialized");
        return DAS_S_OK;
    }
} // namespace

DasResult LocaleSyncCallback::Do()
{
    // Pitfall 1 关键：本方法在 BusinessThread 上执行（PostToBusinessThread
    // 投递），on_after_connect lambda 跑 IO 线程绝不能同步 resolve。locale
    // 逻辑全在此处（BT 上 sync_wait + PumpUntilResponse 安全）。
    auto owner = EnsureLocaleSyncOwner();
    if (owner->started)
    {
        return DAS_S_OK;
    }
    return InitializeLocaleSyncOwner(owner);
}

DAS_CORE_I18N_NS_END

// ----------------------------------------------------------------
// C ABI 工厂（global namespace，照 DasSetDefaultLocale /
// CreateIDasI18nFromDirectory 范式）。DAS_C_API 导出供消费方（DasHostX.exe）
// 跨 DLL 调用——消费方只持有抽象 IDasAsyncCallback*，不引用类 vtable。

DAS_C_API IDasAsyncCallback* MakeLocaleSyncCallback()
{
    auto* p = new Das::Core::i18n::LocaleSyncCallback();
    p->AddRef();
    return p;
}

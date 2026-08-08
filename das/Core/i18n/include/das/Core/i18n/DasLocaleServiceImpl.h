#ifndef DAS_CORE_I18N_DASLOCALESERVICEIMPL_H
#define DAS_CORE_I18N_DASLOCALESERVICEIMPL_H

#include "Config.h"
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasLocaleService.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasLocaleService.Implements.hpp>

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

DAS_CORE_I18N_NS_BEGIN

// IDasLocaleService 主进程实现（照 DasProgressServiceImpl 头文件范式：
// 全部方法内联，vtable 经 COMDAT 落在直接 new 它的消费方 TU，避免跨 DLL
// 链接未导出符号）。
//
// DasHttp 启动时经 RegisterServiceByName("das.locale") 按名注册。前端切语言
// 或插件调 SetDefaultLocale 时：① 调 ::DasSetDefaultLocale 更新进程级
// GlobalLocale；② 反向遍历已注册 callback 触发 OnDefaultLocaleChanged，让
// 远端订阅方（Host 子进程）同步 locale。AddCallback/RemoveCallback 管理跨进程
// callback 生命周期——callbacks_ 存 DasPtr（非裸指针），RemoveCallback erase
// 触发 DasPtr 析构 Release → 远端释放链，防 DOM 泄漏。
//
// @thread_safety 所有公开方法线程安全（callbacks_
// 有互斥保护）。SetDefaultLocale 的反向通知持锁拷贝 callbacks_
// 副本后锁外遍历，防 callback 内回调 service 致 重入死锁。
class DasLocaleServiceImpl final
    : public Das::ExportInterface::DasLocaleServiceImplBase<
          DasLocaleServiceImpl>
{
public:
    DasLocaleServiceImpl() = default;

    DAS_IMPL AddCallback(
        Das::ExportInterface::IDasLocaleCallback* callback) override
    {
        if (!callback)
        {
            return DAS_E_INVALID_POINTER;
        }
        const std::unique_lock lock(mutex_);
        // DasPtr(callback) 构造 AddRef，service 持有独立引用；erase 时
        // Release。
        callbacks_.emplace_back(callback);
        return DAS_S_OK;
    }

    DAS_IMPL RemoveCallback(
        Das::ExportInterface::IDasLocaleCallback* callback) override
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
                const Das::DasPtr<Das::ExportInterface::IDasLocaleCallback>&
                    stored) { return stored.Get() == callback; });
        if (it == callbacks_.end())
        {
            return DAS_E_NOT_FOUND;
        }
        // DasPtr 析构 Release → 触发远端释放链（防 DOM 泄漏）。
        callbacks_.erase(it);
        return DAS_S_OK;
    }

    DAS_IMPL GetDefaultLocale(IDasReadOnlyString** pp_out_locale) override
    {
        if (!pp_out_locale)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_locale = nullptr;
        // 直接委托进程级 GlobalLocale C API（默认 "en"）。
        return ::DasGetDefaultLocale(pp_out_locale);
    }

    DAS_IMPL SetDefaultLocale(IDasReadOnlyString* locale) override
    {
        if (!locale)
        {
            return DAS_E_INVALID_POINTER;
        }

        // ① 更新进程级 GlobalLocale（即时生效，IDasI18n.Translate
        // 默认路径读它）。
        const auto set_result = ::DasSetDefaultLocale(locale);
        if (DAS::IsFailed(set_result))
        {
            return set_result;
        }

        // ② 反向通知所有已注册 callback：持锁拷贝 callbacks_ 副本后锁外遍历，
        // 防 callback 内回调 service 致重入死锁（照 DasProgressServiceImpl
        // 范式）。
        std::vector<Das::DasPtr<Das::ExportInterface::IDasLocaleCallback>>
            snapshot;
        {
            const std::unique_lock lock(mutex_);
            snapshot = callbacks_;
        }

        for (const auto& callback : snapshot)
        {
            const auto rc = callback->OnDefaultLocaleChanged(locale);
            if (DAS::IsFailed(rc))
            {
                // DAS_LOG_ERROR（非 DAS_CORE_LOG_ERROR）：后者引用 libDasCore
                // 内部 g_logger 未导出，跨 DLL 链接失败；照
                // DasProgressServiceImpl 范式用 DAS_FMT_NS::format 生成 const
                // char*。
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "DasLocaleServiceImpl: OnDefaultLocaleChanged failed "
                        "result={}",
                        rc)
                        .c_str());
            }
        }
        return DAS_S_OK;
    }

private:
    std::mutex mutex_;
    std::vector<Das::DasPtr<Das::ExportInterface::IDasLocaleCallback>>
        callbacks_;
};

DAS_CORE_I18N_NS_END

#endif // DAS_CORE_I18N_DASLOCALESERVICEIMPL_H

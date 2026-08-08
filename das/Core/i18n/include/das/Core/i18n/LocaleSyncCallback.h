#ifndef DAS_CORE_I18N_LOCALESYNCCALLBACK_H
#define DAS_CORE_I18N_LOCALESYNCCALLBACK_H

#include <das/DasApi.h>
#include <das/IDasAsyncCallback.h>

// 工厂：构造 locale 同步回调（IDasAsyncCallback）。返回指针已 AddRef，调用方
// 经 PostToBusinessThread 投递到 BusinessThread 执行 Do()。
//
// Do() 在 BT 上跑：resolve "das.locale" → AddCallback(DasLocaleCallbackImpl)
// → GetDefaultLocale → ::DasSetDefaultLocale，把主进程当前 locale 拉到本
// Host 进程的 GlobalLocale（CONTEXT 方案 A'：DasHost 连接 ready 后统一拉
// locale 基础设施）。
//
// 线程约束：on_after_connect lambda 跑 IO 线程，**只**调 PostToBusinessThread
// 异步投 BT，绝不同步 resolve（sync_wait 阻塞 IO → LOOKUP 响应无人收 →
// 死锁，Pitfall 1）。本工厂仅返回回调对象，不做 IPC。
//
// 跨 DLL 安全：DAS_C_API 工厂导出（global namespace，照
// DasSetDefaultLocale / CreateIDasI18nFromDirectory 范式），消费方
// （DasHostX.exe）只持有抽象 IDasAsyncCallback*，不引用类 vtable。
DAS_C_API IDasAsyncCallback* MakeLocaleSyncCallback();

#endif // DAS_CORE_I18N_LOCALESYNCCALLBACK_H

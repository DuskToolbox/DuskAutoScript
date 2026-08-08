#ifndef DAS_CORE_I18N_LOCALESYNCCALLBACK_H
#define DAS_CORE_I18N_LOCALESYNCCALLBACK_H

#include <das/DasExport.h>
#include <das/IDasAsyncCallback.h>

// 创建 locale 同步回调（IDasAsyncCallback）。on_after_connect（在
// SendResponseAsync 之后触发，握手完全结束）经 PostToBusinessThread 投递到
// BusinessThread，Do() 在 BT 上跑 locale 拉取。out-param：调用方经 DasPtr
// 接收，配对无泄漏。PostToBusinessThread 异步投递，不阻塞 IO 线程。
DAS_C_API void CreateLocaleSyncCallback(IDasAsyncCallback** pp_out);

#endif // DAS_CORE_I18N_LOCALESYNCCALLBACK_H

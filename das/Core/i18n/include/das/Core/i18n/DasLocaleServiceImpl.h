#ifndef DAS_CORE_I18N_DASLOCALESERVICEIMPL_H
#define DAS_CORE_I18N_DASLOCALESERVICEIMPL_H

#include "Config.h"
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasLocaleService.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasLocaleService.Implements.hpp>

#include <mutex>
#include <vector>

DAS_CORE_I18N_NS_BEGIN

// IDasLocaleService 主进程实现（照 DasProgressServiceImpl 范式）。
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
        Das::ExportInterface::IDasLocaleCallback* callback) override;
    DAS_IMPL RemoveCallback(
        Das::ExportInterface::IDasLocaleCallback* callback) override;
    DAS_IMPL GetDefaultLocale(IDasReadOnlyString** pp_out_locale) override;
    DAS_IMPL SetDefaultLocale(IDasReadOnlyString* locale) override;

private:
    std::mutex mutex_;
    std::vector<Das::DasPtr<Das::ExportInterface::IDasLocaleCallback>>
        callbacks_;
};

DAS_CORE_I18N_NS_END

#endif // DAS_CORE_I18N_DASLOCALESERVICEIMPL_H

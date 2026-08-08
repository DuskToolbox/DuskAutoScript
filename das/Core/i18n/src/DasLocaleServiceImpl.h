#ifndef DAS_CORE_I18N_SRC_DASLOCALESERVICEIMPL_H
#define DAS_CORE_I18N_SRC_DASLOCALESERVICEIMPL_H

#include <das/Core/i18n/Config.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasLocaleService.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasLocaleService.Implements.hpp>

#include <mutex>
#include <vector>

DAS_CORE_I18N_NS_BEGIN

// IDasLocaleService 主进程实现（Core 内部，不暴露 public header）。
// DasHttp 经 CreateDasLocaleServiceImpl factory 创建，RegisterServiceByName
// 注册 "das.locale"。前端切语言 / 插件 SetDefaultLocale：①
// ::DasSetDefaultLocale 更新进程 GlobalLocale；② 持锁拷贝 callbacks_
// 副本后锁外遍历触发 OnDefaultLocaleChanged（防重入死锁）。callbacks_ 存
// DasPtr（非裸指针）， RemoveCallback erase 触发 Release → 远端释放链。
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

#endif // DAS_CORE_I18N_SRC_DASLOCALESERVICEIMPL_H

#ifndef DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLI18N_H
#define DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLI18N_H

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasI18n.h>

#include <string>
#include <string_view>

DAS_NS_BEGIN

namespace Plugins::DasFlowControl
{
    // 插件 i18n 表查询。生产路径惰性装载 <plugin_dir>/i18n/DasFlowControl
    // （经 boost::dll::this_line_location 推导，cwd != 插件安装目录；每插件
    // 子目录避免与其他插件的同名翻译表互覆），装载失败永久降级硬编码英文；
    // 测试经 SetFlowControlI18nForTest 注入替身表（优先于目录装载，验证
    // key 解析链路）。
    ExportInterface::IDasI18n* GetFlowControlI18n();

    // 测试缝：注入替身翻译表（传 nullptr 清除，恢复默认装载路径）。
    void SetFlowControlI18nForTest(ExportInterface::IDasI18n* i18n);

    // key → 翻译文本；表缺失或 key 未命中时返回 fallback_text。
    std::string TranslateFlowControlKey(
        const char*      key,
        std::string_view fallback_text);
}

DAS_NS_END

#endif // DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLI18N_H

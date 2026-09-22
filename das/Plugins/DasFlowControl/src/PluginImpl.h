#ifndef DAS_PLUGINS_DASFLOWCONTROL_PLUGINIMPL_H
#define DAS_PLUGINS_DASFLOWCONTROL_PLUGINIMPL_H

#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>

DAS_NS_BEGIN

class DasFlowControlPlugin final
    : public PluginInterface::DasPluginPackageImplBase<DasFlowControlPlugin>
{
public:
    DAS_IMPL EnumFeature(
        size_t                             index,
        PluginInterface::DasPluginFeature* p_out_feature) override;

    DAS_IMPL CreateFeatureInterface(size_t index, IDasBase** pp_out_interface)
        override;

    // 加载器顺序契约（注入先于枚举）仍需本方法可达；流程组件不消费
    // creator（调用中介组件退役后组件工厂零宿主依赖），接受注入即丢弃。
    DAS_IMPL SetTaskComponentCreator(
        PluginInterface::IDasTaskComponentCreator* /*p_host*/) override
    {
        return DAS_S_OK;
    }
};

DAS_NS_END

#endif // DAS_PLUGINS_DASFLOWCONTROL_PLUGINIMPL_H

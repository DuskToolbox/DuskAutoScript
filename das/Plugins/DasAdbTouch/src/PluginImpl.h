#ifndef DAS_PLUGINS_DASADBTOUCH_PLUGINIMPL_H
#define DAS_PLUGINS_DASADBTOUCH_PLUGINIMPL_H

#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>

DAS_NS_BEGIN

class DasAdbTouchPlugin final
    : public PluginInterface::DasPluginPackageImplBase<DasAdbTouchPlugin>
{

    DasPtr<DAS::PluginInterface::IDasBasicErrorLens> g_error_lens;
    DasPtr<PluginInterface::IDasTaskComponentHost>   task_component_host_;

public:
    DAS_UTILS_IDASBASE_AUTO_IMPL(DasAdbTouchPlugin)
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_object) override;
    // IDasPluginPackage
    DAS_IMPL EnumFeature(
        size_t                             index,
        PluginInterface::DasPluginFeature* p_out_feature) override;
    DAS_IMPL CreateFeatureInterface(uint64_t index, IDasBase** pp_out_interface)
        override;

    // 存储加载器注入的 task component host（顺序契约：注入先于枚举）。
    DAS_IMPL SetTaskComponentHost(
        PluginInterface::IDasTaskComponentHost* p_host) override
    {
        task_component_host_ =
            DasPtr<PluginInterface::IDasTaskComponentHost>(p_host);
        return DAS_S_OK;
    }

    PluginInterface::IDasTaskComponentHost* GetTaskComponentHost() const
    {
        return task_component_host_.Get();
    }
};

DAS_NS_END

#endif // DAS_PLUGINS_DASADBTOUCH_PLUGINIMPL_H

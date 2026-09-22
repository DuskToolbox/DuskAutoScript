#ifndef DAS_PLUGINS_DASMAAPI_PLUGINIMPL_H
#define DAS_PLUGINS_DASMAAPI_PLUGINIMPL_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    class DasMaaPiPlugin final
        : public PluginInterface::DasPluginPackageImplBase<DasMaaPiPlugin>
    {
        DasPtr<PluginInterface::IDasTaskComponentCreator> task_component_host_;

    public:
        DAS_IMPL EnumFeature(
            size_t                             index,
            PluginInterface::DasPluginFeature* p_out_feature) override;

        DAS_IMPL CreateFeatureInterface(
            size_t     index,
            IDasBase** pp_out_interface) override;

        // 存储加载器注入的 task component host（顺序契约：注入先于枚举）。
        DAS_IMPL SetTaskComponentCreator(
            PluginInterface::IDasTaskComponentCreator* p_host) override
        {
            task_component_host_ =
                DasPtr<PluginInterface::IDasTaskComponentCreator>(p_host);
            return DAS_S_OK;
        }

        PluginInterface::IDasTaskComponentCreator* GetTaskComponentCreator()
            const
        {
            return task_component_host_.Get();
        }
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_PLUGINIMPL_H

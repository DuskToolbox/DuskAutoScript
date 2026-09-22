#ifndef DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H
#define DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H

#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

namespace Das::Plugins::DasGraphTask
{

    // 工厂经构造参数持有 host（包在 CreateFeatureInterface 时以
    // GetTaskComponentCreator() 注入——包自有存储由加载器先于枚举写入）。
    class DasGraphTaskComponentFactory final
        : public Das::PluginInterface::DasTaskComponentFactoryImplBase<
              DasGraphTaskComponentFactory>
    {
        DasPtr<Das::PluginInterface::IDasTaskComponentCreator> host_;

    public:
        explicit DasGraphTaskComponentFactory(
            Das::PluginInterface::IDasTaskComponentCreator* p_host);

        DAS_IMPL CreateComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component)
            override;
    };

    class DasGraphTaskPluginPackage final
        : public Das::PluginInterface::DasPluginPackageImplBase<
              DasGraphTaskPluginPackage>
    {
        DasPtr<Das::PluginInterface::IDasTaskComponentCreator>
            task_component_host_;

    public:
        DasGraphTaskPluginPackage() = default;

        DAS_IMPL EnumFeature(
            size_t                                  index,
            Das::PluginInterface::DasPluginFeature* p_out_feature) override;

        DAS_IMPL CreateFeatureInterface(
            size_t     index,
            IDasBase** pp_out_interface) override;

        // 存储加载器注入的 task component host（顺序契约：注入先于枚举）。
        DAS_IMPL SetTaskComponentCreator(
            Das::PluginInterface::IDasTaskComponentCreator* p_host) override
        {
            task_component_host_ =
                DasPtr<Das::PluginInterface::IDasTaskComponentCreator>(p_host);
            return DAS_S_OK;
        }

        Das::PluginInterface::IDasTaskComponentCreator*
        GetTaskComponentCreator() const
        {
            return task_component_host_.Get();
        }
    };

} // namespace Das::Plugins::DasGraphTask

#endif // DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H

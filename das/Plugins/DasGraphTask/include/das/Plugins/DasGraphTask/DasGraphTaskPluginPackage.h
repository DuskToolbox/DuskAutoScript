#ifndef DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H
#define DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H

#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

namespace Das::Plugins::DasGraphTask
{

    // 工厂经构造参数持有 host（包在 CreateFeatureInterface 时以
    // GetTaskComponentHost() 注入——包默认存储由加载器先于枚举写入）。
    class DasGraphTaskComponentFactory final
        : public Das::PluginInterface::DasTaskComponentFactoryImplBase<
              DasGraphTaskComponentFactory>
    {
        DasPtr<Das::PluginInterface::IDasTaskComponentHost> host_;

    public:
        explicit DasGraphTaskComponentFactory(
            Das::PluginInterface::IDasTaskComponentHost* p_host);

        DAS_IMPL CreateComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component)
            override;
    };

    class DasGraphTaskPluginPackage final
        : public Das::PluginInterface::DasPluginPackageImplBase<
              DasGraphTaskPluginPackage>
    {
    public:
        DasGraphTaskPluginPackage() = default;

        DAS_IMPL EnumFeature(
            size_t                                  index,
            Das::PluginInterface::DasPluginFeature* p_out_feature) override;

        DAS_IMPL CreateFeatureInterface(
            size_t     index,
            IDasBase** pp_out_interface) override;
    };

} // namespace Das::Plugins::DasGraphTask

#endif // DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKPLUGINPACKAGE_H

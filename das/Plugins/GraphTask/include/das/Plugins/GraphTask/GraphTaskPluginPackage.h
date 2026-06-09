#ifndef DAS_PLUGINS_GRAPHTASK_GRAPHTASKPLUGINPACKAGE_H
#define DAS_PLUGINS_GRAPHTASK_GRAPHTASKPLUGINPACKAGE_H

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>

#include <atomic>

namespace Das::Plugins::GraphTask
{

    class GraphTaskPluginPackage final
        : public Das::PluginInterface::IDasPluginPackage,
          public Das::PluginInterface::IDasTaskComponentFactory
    {
        DasPtr<Das::PluginInterface::IDasTaskComponentHost> host_;
        std::atomic<uint32_t>                               ref_count_{0};

    public:
        GraphTaskPluginPackage() = default;

        // --- IUnknown ---
        uint32_t DAS_STD_CALL AddRef() override;
        uint32_t DAS_STD_CALL Release() override;
        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override;

        // --- IDasPluginPackage ---
        DAS_IMPL EnumFeature(
            size_t                                  index,
            Das::PluginInterface::DasPluginFeature* p_out_feature) override;

        DAS_IMPL CreateFeatureInterface(
            size_t     index,
            IDasBase** pp_out_interface) override;

        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;

        DAS_IMPL CanUnloadNow(bool* p_can_unload) override;

        // --- IDasTaskComponentFactory ---
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL CreateComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component)
            override;

        DAS_IMPL SetTaskComponentHost(
            Das::PluginInterface::IDasTaskComponentHost* p_host) override;
    };

} // namespace Das::Plugins::GraphTask

#endif // DAS_PLUGINS_GRAPHTASK_GRAPHTASKPLUGINPACKAGE_H

#ifndef DAS_PLUGINS_DASMAAPI_MAAPIAGENTCOMPONENTFACTORY_H
#define DAS_PLUGINS_DASMAAPI_MAAPIAGENTCOMPONENTFACTORY_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasComponentFactory.Implements.hpp>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiAgentComponentFactory,
    0x69f20005,
    0x0000,
    0x4000,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x01);

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    class MaapiAgentComponentFactory final
        : public PluginInterface::DasComponentFactoryImplBase<
              MaapiAgentComponentFactory>
    {
    public:
        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL IsSupported(const DasGuid& component_iid) override;
        DAS_IMPL CreateInstance(
            const DasGuid&                   component_iid,
            PluginInterface::IDasComponent** pp_out_component) override;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPIAGENTCOMPONENTFACTORY_H

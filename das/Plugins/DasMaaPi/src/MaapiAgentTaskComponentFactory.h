#ifndef DAS_PLUGINS_DASMAAPI_MAAPIAGENTTASKCOMPONENTFACTORY_H
#define DAS_PLUGINS_DASMAAPI_MAAPIAGENTTASKCOMPONENTFACTORY_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiAgentTaskComponentFactory,
    0x69f20007,
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
    class MaapiAgentTaskComponentFactory final
        : public PluginInterface::DasTaskComponentFactoryImplBase<
              MaapiAgentTaskComponentFactory>
    {
    public:
        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL CreateComponent(
            const DasGuid&                       component_guid,
            PluginInterface::IDasTaskComponent** pp_out_component) override;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPIAGENTTASKCOMPONENTFACTORY_H

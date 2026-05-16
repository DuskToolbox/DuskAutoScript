#ifndef DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSIONFACTORY_H
#define DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSIONFACTORY_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSessionFactory.Implements.hpp>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiAuthoringSessionFactory,
    0x69f20002,
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
    class MaapiAuthoringSessionFactory final
        : public PluginInterface::DasTaskAuthoringSessionFactoryImplBase<
              MaapiAuthoringSessionFactory>
    {
    public:
        DAS_IMPL CreateSession(
            const DasGuid&                              task_guid,
            ExportInterface::IDasJson*                  p_context_json,
            PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSIONFACTORY_H

#ifndef DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKAUTHORINGSESSIONFACTORY_H
#define DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKAUTHORINGSESSIONFACTORY_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSessionFactory.Implements.hpp>

// Authoring factory GUID: {EFC3D51E-B707-4702-97ED-8ADA923E2BF0}
// Must match DasGraphTask.json tasks.<id>.authoring.factoryGuid for manifest
// lookup (DAS-77).
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    DasGraphTaskAuthoringSessionFactory,
    0xEFC3D51E,
    0xB707,
    0x4702,
    0x97,
    0xED,
    0x8A,
    0xDA,
    0x92,
    0x3E,
    0x2B,
    0xF0);

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    // IDasTaskAuthoringSessionFactory for the formSequence authoring kind.
    // Mirrors DasMaaPi's MaapiAuthoringSessionFactory (DAS-77).
    class DasGraphTaskAuthoringSessionFactory final
        : public PluginInterface::DasTaskAuthoringSessionFactoryImplBase<
              DasGraphTaskAuthoringSessionFactory>
    {
    public:
        DAS_IMPL CreateSession(
            const DasGuid&                              task_guid,
            ExportInterface::IDasJson*                  p_context_json,
            PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override;
    };
} // namespace Plugins::DasGraphTask
DAS_NS_END

#endif // DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKAUTHORINGSESSIONFACTORY_H

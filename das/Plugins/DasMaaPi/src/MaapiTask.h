#ifndef DAS_PLUGINS_DASMAAPI_MAAPITASK_H
#define DAS_PLUGINS_DASMAAPI_MAAPITASK_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTask.Implements.hpp>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiTask,
    0x69f20001,
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
    class MaapiTask final : public PluginInterface::DasTaskImplBase<MaapiTask>
    {
    public:
        DAS_IMPL Do(
            PluginInterface::IDasStopToken* stop_token,
            ExportInterface::IDasJson*      p_environment_json,
            ExportInterface::IDasJson*      p_task_settings_json) override;

        DAS_IMPL GetNextExecutionTime(
            ExportInterface::DasDate* p_out_date) override;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPITASK_H

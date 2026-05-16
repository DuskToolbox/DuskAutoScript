#ifndef DAS_PLUGINS_DASMAAPI_PLUGINIMPL_H
#define DAS_PLUGINS_DASMAAPI_PLUGINIMPL_H

#include <das/Plugins/DasMaaPi/AuthoringProjector.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTask.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSession.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSessionFactory.Implements.hpp>

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

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiAuthoringSession,
    0x69f20003,
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

    class MaapiAuthoringSession final
        : public PluginInterface::DasTaskAuthoringSessionImplBase<
              MaapiAuthoringSession>
    {
    public:
        MaapiAuthoringSession() = default;
        explicit MaapiAuthoringSession(AcceptedSettingsDto settings);

        DAS_IMPL GetDocument(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_document_json) override;
        DAS_IMPL ApplyChange(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;
        DAS_IMPL Compile(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;

    private:
        AcceptedSettingsDto settings_{};
        int64_t             revision_ = 0;
    };

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

    class DasMaaPiPlugin final
        : public PluginInterface::DasPluginPackageImplBase<DasMaaPiPlugin>
    {
    public:
        DAS_IMPL EnumFeature(
            size_t                             index,
            PluginInterface::DasPluginFeature* p_out_feature) override;

        DAS_IMPL CreateFeatureInterface(
            size_t     index,
            IDasBase** pp_out_interface) override;

        DAS_IMPL CanUnloadNow(bool* p_can_unload) override;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_PLUGINIMPL_H

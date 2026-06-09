#ifndef DAS_PLUGINS_DASMAAPI_MAAPIRUNTASKCOMPONENT_H
#define DAS_PLUGINS_DASMAAPI_MAAPIRUNTASKCOMPONENT_H

#include <cassert>

#include <cpp_yyjson.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiRunTaskComponent,
    0x69f20008,
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
    MaapiRunTaskComponentFactory,
    0x69f20009,
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
    struct MaapiRunTaskDiagnosticDto
    {
        std::string                 severity;
        std::string                 code;
        std::string                 message;
        std::optional<std::string>  path;
        std::optional<std::int64_t> provider_code;
    };

    struct MaapiRunTaskOutputsDto
    {
        std::vector<std::string> completed_tasks;
        bool                     stopped = false;
    };

    struct MaapiRunTaskSignalsDto
    {
        bool succeeded = false;
        bool failed = false;
        bool cancelled = false;
    };

    struct MaapiRunTaskResultDto
    {
        int32_t                                version = 1;
        std::string                            status;
        MaapiRunTaskOutputsDto                 outputs;
        std::vector<MaapiRunTaskDiagnosticDto> diagnostics;
        MaapiRunTaskSignalsDto                 signals;
    };

    class MaapiRunTaskComponent final
        : public PluginInterface::DasTaskComponentImplBase<
              MaapiRunTaskComponent>
    {
    public:
        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL ApplySettingsChange(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;

        DAS_IMPL Do(
            PluginInterface::IDasStopToken* stop_token,
            ExportInterface::IDasJson*      p_environment_json,
            ExportInterface::IDasJson*      p_settings_json,
            ExportInterface::IDasJson*      p_input_json,
            ExportInterface::IDasJson**     pp_out_result_json) override;
    };

    class MaapiRunTaskComponentFactory final
        : public PluginInterface::DasTaskComponentFactoryImplBase<
              MaapiRunTaskComponentFactory>
    {
        DasPtr<PluginInterface::IDasTaskComponentHost> host_;

    public:
        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL CreateComponent(
            const DasGuid&                       component_guid,
            PluginInterface::IDasTaskComponent** pp_out_component) override;

        DAS_IMPL SetTaskComponentHost(
            PluginInterface::IDasTaskComponentHost* p_host) override;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::MaapiRunTaskDiagnosticDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiRunTaskOutputsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiRunTaskSignalsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiRunTaskResultDto>
{
    using type = yyjson::snake_to_camel_transform;
};

#endif // DAS_PLUGINS_DASMAAPI_MAAPIRUNTASKCOMPONENT_H

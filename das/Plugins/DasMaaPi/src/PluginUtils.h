#ifndef DAS_PLUGINS_DASMAAPI_PLUGINUTILS_H
#define DAS_PLUGINS_DASMAAPI_PLUGINUTILS_H

#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Plugins/DasMaaPi/AuthoringProjector.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/CommonUtils.hpp>

#include <map>
#include <optional>
#include <vector>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    DasPtr<ExportInterface::IDasJson> WrapJson(yyjson::value value);
    yyjson::value                     JsonString(std::string_view value);
    std::optional<yyjson::value>      ReadJson(ExportInterface::IDasJson* json);

    std::optional<PiCatalog> TryParseCatalog(
        const AcceptedSettingsDto&    settings,
        std::vector<PiDiagnosticDto>& diagnostics);

    yyjson::value BuildDocument(
        const AcceptedSettingsDto&    settings,
        int64_t                       revision,
        std::vector<PiDiagnosticDto>& diagnostics);

    yyjson::value BuildApplyResult(
        const AcceptedSettingsDto&   settings,
        int64_t                      revision,
        std::vector<PiDiagnosticDto> diagnostics);

    yyjson::value MakeAdapterOnlyDocument();

    // Port derivation utilities (D-05/D-06/D-07)

    /// Derive graph port definitions from PI catalog for a given task (D-05).
    std::vector<Core::GraphRuntime::Dto::GraphPortDefinitionDto>
    DerivePortDefinitions(
        const PiCatalog&   catalog,
        const std::string& task_name);

    /// Build graph_port_id → pi_param_name mapping table (D-06).
    std::map<std::string, std::string> DerivePortMap(
        const PiCatalog&   catalog,
        const std::string& task_name);

    /// Serialize port definitions to a yyjson JSON array (D-07).
    yyjson::value BuildPortDefinitionsJson(
        const std::vector<Core::GraphRuntime::Dto::GraphPortDefinitionDto>&
            ports);

} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_PLUGINUTILS_H

#ifndef DAS_PLUGINS_DASMAAPI_PLUGINUTILS_H
#define DAS_PLUGINS_DASMAAPI_PLUGINUTILS_H

#include <das/Plugins/DasMaaPi/AuthoringProjector.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/CommonUtils.hpp>

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
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_PLUGINUTILS_H

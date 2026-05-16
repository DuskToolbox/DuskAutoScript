#pragma once

#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/ExecutionEnvelope.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>

namespace Das::Plugins::DasMaaPi
{
    CompileResultDto CompileMaapi(
        const AcceptedSettingsDto& settings,
        const PiCatalog&           catalog,
        std::string_view           purpose);

    yyjson::value SerializeCompileResult(
        const CompileResultDto& result,
        std::string_view        purpose);
} // namespace Das::Plugins::DasMaaPi

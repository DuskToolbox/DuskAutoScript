#pragma once

#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>

#include <optional>

namespace Das::Plugins::DasMaaPi
{
    AcceptedSettingsDto ParseAcceptedSettings(const yyjson::value& value);
    yyjson::value       SerializeAcceptedSettings(
              const AcceptedSettingsDto& settings);

    yyjson::value ProjectAuthoringDocument(
        const AcceptedSettingsDto&             settings,
        const PiCatalog*                       catalog,
        const std::vector<PiDiagnosticDto>&    diagnostics,
        int64_t                                revision);

    void ApplySetValueChange(
        AcceptedSettingsDto& settings,
        const yyjson::value& change,
        const PiCatalog*     catalog = nullptr);

    void ApplyPreset(
        AcceptedSettingsDto& settings,
        const PiCatalog&     catalog,
        std::string_view     preset_name);
} // namespace Das::Plugins::DasMaaPi

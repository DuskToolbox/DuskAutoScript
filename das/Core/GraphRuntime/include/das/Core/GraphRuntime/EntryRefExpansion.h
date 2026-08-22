#ifndef DAS_CORE_GRAPHRUNTIME_ENTRYREFEXPANSION_H
#define DAS_CORE_GRAPHRUNTIME_ENTRYREFEXPANSION_H

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

/// Outcome of resolving one entryRef node's repository entry into an
/// executable child snapshot.
struct EntryRefResolution
{
    bool ok = false;
    /// Component guid installed at the expanded node — the resolver decides
    /// which wrapper component consumes the snapshot (production resolves to
    /// the repository-invoke flow component).
    std::string component_guid;
    /// Resolved child execution snapshot. The expander serialises it into a
    /// broadcast binding default so the runtime materialises it on the
    /// node's compiledSnapshot input port.
    yyjson::value snapshot_json;
    /// Human-readable failure reasons; passed through to the caller's
    /// diagnostics on failure.
    std::vector<std::string> diagnostics;
};

/// Resolves one repository entry (entry id + optional expected revision)
/// into an executable snapshot. Injected by the caller — the expander itself
/// carries no repository knowledge.
using EntryRefResolver =
    std::function<EntryRefResolution(int64_t, const std::optional<int64_t>&)>;

/// Expand every entryRef marker node in a compiled plan into a concrete
/// component node: rewrites component_guid from the resolution, clears the
/// entry marker payload, and appends a broadcast binding that delivers the
/// resolved snapshot (as a JSON string default) to the node's
/// compiledSnapshot input port. Broadcast bindings create no data
/// dependency, so execution_order is untouched. A resolver failure is
/// fail-closed: the marker node stays as-is, its diagnostics are appended
/// to out_diagnostics (when provided) and the call returns DAS_E_FAIL.
/// Plans without entry markers are returned unchanged (idempotent no-op).
DasResult ExpandEntryRefNodes(
    Dto::CompiledGraphPlanDto& plan,
    const EntryRefResolver&    resolver,
    std::vector<std::string>*  out_diagnostics = nullptr);

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_ENTRYREFEXPANSION_H

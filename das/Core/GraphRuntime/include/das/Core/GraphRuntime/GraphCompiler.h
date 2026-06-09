#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHCOMPILER_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHCOMPILER_H

#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphDocument.h>

#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration — avoid pulling full ForeignInterfaceHost header.
DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN
class TaskComponentFactoryManager;
DAS_CORE_FOREIGNINTERFACEHOST_NS_END

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// Import forward-declared type into this namespace for convenience.
using ForeignInterfaceHost::TaskComponentFactoryManager;

// ---------------------------------------------------------------------------
// Diagnostic types for compile-time edge validation
// ---------------------------------------------------------------------------

enum class CompileDiagnosticKind
{
    PortNotFound,              // Edge references a port_id not in the manifest
    TypeMismatch,              // Source port type incompatible with target
    NodeNotFound,              // Edge references a node_id not in the graph
    UnknownPortType,           // Port type string not in known set (warning)
    UnresolvableComponentGuid, // component_guid not in factory manager
    CyclicEntryRef,            // entryRef cycle detected (reserved for 02-10)
    CyclicEdgeGraph // Edge-based cycle detected during topological sort
};

struct CompileEdgeDiagnostic
{
    CompileDiagnosticKind kind = CompileDiagnosticKind::PortNotFound;
    std::string           node_id; // Node this diagnostic refers to
    std::string           port_id; // Port (empty for node-level diagnostics)
    std::string           edge_id; // Edge (empty if not edge-specific)
    std::string           message; // Human-readable message (English)

    enum class PortDirection
    {
        Input,
        Output,
        Unknown
    };
    PortDirection direction = PortDirection::Unknown;

    // For TypeMismatch: expected (target input) vs actual (source output)
    std::string expected_type;
    std::string actual_type;
};

// ---------------------------------------------------------------------------
// GraphCompiler — compile-layer service for manifest reading + edge validation
// ---------------------------------------------------------------------------

class GraphCompiler
{
public:
    GraphCompiler();

    /// Set the factory manager for manifest resolution. Must be called before
    /// ValidateEdgePorts() or ReadManifest().
    void SetFactoryManager(TaskComponentFactoryManager* factory_manager);

    // -----------------------------------------------------------------------
    // Port entry types returned by ReadManifest
    // -----------------------------------------------------------------------
    struct PortEntry
    {
        std::string port_id;
        std::string port_type;
    };

    struct ManifestPorts
    {
        std::vector<PortEntry> inputs;
        std::vector<PortEntry> outputs;
        bool                   was_resolved = false;
    };

    /// Read manifest definition.inputs/outputs for a given component_guid.
    /// Returns {inputs, outputs} port lists. Empty vectors if not found.
    ManifestPorts ReadManifest(const std::string& component_guid) const;

    /// Validate all edges in a GraphDocumentDto against node manifests.
    /// For each edge:
    ///   1. Verify source_node_id and target_node_id exist in graph
    ///   2. Resolve source node manifest -> check source_port_id in outputs
    ///   3. Resolve target node manifest -> check target_port_id in inputs
    ///   4. Check type compatibility between source output and target input
    /// Returns: vector of diagnostics (empty if all edges valid).
    std::vector<CompileEdgeDiagnostic> ValidateEdgePorts(
        const Dto::GraphDocumentDto& document);

    /// Compute topological order of nodes based on edge dependencies.
    /// Uses Kahn's algorithm (BFS). Returns empty vector if cycle detected.
    std::vector<std::string> ComputeExecutionOrder(
        const Dto::GraphDocumentDto& document);

    /// Generate port binding plan from edges + manifest port type info.
    /// Each edge becomes one PortBindingDto with expected_type from source.
    Dto::PortBindingPlanDto GeneratePortBindingPlan(
        const Dto::GraphDocumentDto& document);

    /// Detect cycles in entryRef references.
    /// Scans all nodes with target.kind="entryRef" and checks for
    /// self-reference or recursive references (via visited set).
    std::vector<CompileEdgeDiagnostic> DetectCyclicEntryRefs(
        const Dto::GraphDocumentDto& document);

    /// Unified compile entry point: runs all phases and produces a
    /// CompiledGraphPlanDto. Steps: ValidateEdgePorts ->
    /// ComputeExecutionOrder -> GeneratePortBindingPlan ->
    /// DetectCyclicEntryRefs -> generate compiled_fingerprint.
    Dto::CompiledGraphPlanDto Compile(const Dto::GraphDocumentDto& document);

private:
    TaskComponentFactoryManager* factory_manager_ = nullptr;

    /// Check type compatibility between source output and target input.
    static bool IsTypeCompatible(
        const std::string& source_type,
        const std::string& target_type);

    /// Resolve port entries from a manifest definition yyjson::value.
    static std::vector<PortEntry> PortsFromDefinitionList(
        const yyjson::value& definition,
        const std::string&   list_key); // "inputs" or "outputs"

    /// Build a node index: node_id -> GraphNodeDto* for O(1) lookup.
    static std::unordered_map<std::string, const Dto::GraphNodeDto*>
    BuildNodeIndex(const Dto::GraphDocumentDto& document);

    /// Generate a compiled fingerprint from document metadata + execution
    /// order.
    static std::string GenerateCompiledFingerprint(
        const std::string&              document_id,
        int32_t                         source_revision,
        const std::vector<std::string>& execution_order);

    /// Convert a CompileEdgeDiagnostic to a yyjson::value for plan
    /// diagnostics.
    static yyjson::value DiagnosticToJson(const CompileEdgeDiagnostic& diag);
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHCOMPILER_H

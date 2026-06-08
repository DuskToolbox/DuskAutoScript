#ifndef DAS_CORE_GRAPHRUNTIME_EXTRACTSUBGRAPH_H
#define DAS_CORE_GRAPHRUNTIME_EXTRACTSUBGRAPH_H

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphEntryId.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

/// Records a port generated at the subgraph boundary for a single
/// edge crossing the selection boundary.
struct ExtractPortMapping
{
    std::string old_parent_node_id;
    std::string old_parent_port_id;
    std::string child_port_id;
    std::string port_type; // matches GraphPortDefinitionDto::port_type
};

/// Result of an Extract operation (02-19: construction only).
/// 02-20 consumes node_id_mapping, input/output_port_mappings
/// to replace selected nodes with entryRef in the parent.
struct ExtractResult
{
    GraphEntryId                       child_entry_id = 0;
    int                                revision = 0;
    std::string                        fingerprint;
    Dto::GraphDocumentDto              child_graph_document;
    std::map<std::string, std::string> node_id_mapping; // old → new
    std::vector<ExtractPortMapping>    input_port_mappings;
    std::vector<ExtractPortMapping>    output_port_mappings;
    std::vector<std::string>           diagnostics;
};

/// ExtractSubgraph: Transaction construction for the v23 Extract
/// Subgraph workflow. Constructs a child GraphDocument from selected
/// nodes, classifies edges, generates boundary ports, creates a child
/// entry, and compiles it.
///
/// 02-19 handles construction (this plan).
/// 02-20 handles parent replacement + undo.
class ExtractSubgraph
{
public:
    ExtractSubgraph() = default;

    using EntryFactory = std::function<GraphEntryId(
        const Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto&)>;

    using CompileCallback =
        std::function<Dto::CompiledGraphPlanDto(GraphEntryId)>;

    ExtractResult Extract(
        GraphEntryId                 parent_entry_id,
        const std::set<std::string>& selected_node_ids,
        const Dto::GraphDocumentDto& parent_graph_doc,
        EntryFactory                 entry_factory,
        CompileCallback              compile_callback) const;

private:
    static std::string MakeChildNodeId(const std::string& old_node_id);

    static std::string InferPortType(
        const std::string&                              port_id,
        const std::vector<Dto::GraphPortDefinitionDto>& port_defs);
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_EXTRACTSUBGRAPH_H

#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHAUTHORING_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHAUTHORING_H

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphDocument.h>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// ---------------------------------------------------------------------------
// Authoring change request types (v21/v33 — authoring-only, never runtime)
// ---------------------------------------------------------------------------
// Each variant represents a single structured mutation that an authoring
// client may submit.  ApplySettingsChange validates, normalises and applies
// the mutation to a GraphDocumentDto in-place.
// ---------------------------------------------------------------------------

/// Request to add a new node to the graph.
struct AddNodeChange
{
    Dto::GraphNodeDto node;
};

/// Request to remove a node (and all its connected edges) from the graph.
struct RemoveNodeChange
{
    std::string node_id;
};

/// Request to connect two ports with a new edge.
struct ConnectPortsChange
{
    Dto::GraphEdgeDto edge;
};

/// Request to disconnect (remove) an existing edge.
struct DisconnectPortsChange
{
    std::string edge_id;
};

/// Request to update the settings blob on an existing node.
struct UpdateNodeConfigChange
{
    std::string   node_id;
    yyjson::value settings;
};

/// Tag-dispatched union of all authoring change types.
struct GraphAuthoringChange
{
    std::variant<
        AddNodeChange,
        RemoveNodeChange,
        ConnectPortsChange,
        DisconnectPortsChange,
        UpdateNodeConfigChange>
        payload;
};

// ---------------------------------------------------------------------------
// Authoring result
// ---------------------------------------------------------------------------

enum class AuthoringErrorKind
{
    None,            // Success
    NodeNotFound,    // Referenced node_id does not exist
    DuplicateNodeId, // addNode with an already-existing node_id
    DuplicateEdgeId, // connectPorts with an already-existing edge_id
    EdgeNotFound,    // disconnectPorts edge_id does not exist
    EmptyNodeId,     // node_id string is empty
    EmptyEdgeId,     // edge_id string is empty
    InvalidEdge,     // Edge references non-existent source/target node
    NoChange,        // UpdateNodeConfig with identical settings
};

struct AuthoringResult
{
    AuthoringErrorKind error_kind = AuthoringErrorKind::None;
    std::string        message;

    [[nodiscard]]
    bool Ok() const noexcept
    {
        return error_kind == AuthoringErrorKind::None;
    }
};

// ---------------------------------------------------------------------------
// ApplySettingsChange — validate, normalise, apply a single mutation
// ---------------------------------------------------------------------------

/// Validate and apply a single GraphAuthoringChange to `document` in-place.
///
/// Validation rules:
///   addNode         — node_id must be non-empty and unique
///   removeNode      — node_id must exist; cascades edge removal
///   connectPorts    — edge_id must be non-empty and unique;
///                     source/target nodes must exist
///   disconnectPorts — edge_id must exist
///   updateNodeConfig — node_id must exist
///
/// On success returns {None, ""}.
/// On failure returns the error kind and a human-readable message;
/// the document is NOT modified.
AuthoringResult ApplySettingsChange(
    Dto::GraphDocumentDto&      document,
    const GraphAuthoringChange& change);

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHAUTHORING_H

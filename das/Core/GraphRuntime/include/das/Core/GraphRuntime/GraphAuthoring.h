#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHAUTHORING_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHAUTHORING_H

#include <cassert>
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

// ---------------------------------------------------------------------------
// FromSequence lint — optional, non-blocking topology hints (DAS-75)
// ---------------------------------------------------------------------------
//
// A fromsequence authoring doc is a linear signal-chain graph. The authoring
// layer intentionally does NOT hard-validate linearity — that is a frontend
// constraint (lenient validation). This lint is an OPTIONAL, non-blocking pass
// that surfaces topology hints a UI may show as warnings.
//
// It only produces output when the document is tagged "fromsequence", and it
// NEVER rejects a document (warnings only). It is NOT invoked by GraphCompiler
// or GraphRuntime — those ignore the tag entirely.

struct FromSequenceLintWarning
{
    std::string node_id; // empty for graph-level warnings
    std::string message;
};

/// Optional, non-blocking lint for a fromsequence-tagged document.
///
/// Returns topology warnings when the signal edges do not form a single linear
/// chain (a node that fans out / merges, or a topology that splits into
/// multiple disconnected pieces). Returns an empty vector when the document is
/// not tagged "fromsequence" or has nothing to warn about.
std::vector<FromSequenceLintWarning> LintFromSequenceLinearity(
    const Dto::GraphDocumentDto& document);

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHAUTHORING_H

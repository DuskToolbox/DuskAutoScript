#ifndef DAS_CORE_GRAPHRUNTIME_AUTHORINGDOCCONTRACT_H
#define DAS_CORE_GRAPHRUNTIME_AUTHORINGDOCCONTRACT_H

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphDocument.h>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// ---------------------------------------------------------------------------
// AuthoringDocContract (DAS-77)
// ---------------------------------------------------------------------------
//
// The external-facing authoring document contract — the stable shape exposed
// across the session ABI boundary. It mirrors the TaskAuthoringHttpContract
// schema draft (AuthoringDocument / GraphNode / GraphConnection /
// SequenceItem), NOT the internal GraphDocumentDto. GraphDocumentDto remains
// the session's authoritative primary store; this contract is its public
// projection.
//
// The contract deliberately drops runtime-private fields:
//   - GraphEdgeDto::edge_id        (not in GraphConnection)
//   - GraphEdgeDto::edge_type      (signal/data is a runtime concern)
//   - GraphNodeDto::dynamic_ports  (runtime port binding)
//   - GraphNodeDto::target_kind / entry_ref internals (componentGuid only)
//
// kind dispatch:
//   - kind == FormSequence  ⇔  the GraphDocumentDto carries the linear marker
//                               tag (kLinearTag). GetDocument then reverse-
//                               projects the linear signal chain into a flat
//                               SequenceItem[] (a view of the same store).
//   - kind == Graph         ⇔  the GraphDocumentDto is in canvas mode (no
//                               linear tag). GetDocument thin-maps nodes and
//                               connections, dropping the private fields.
// ---------------------------------------------------------------------------

namespace Contract
{
    /// Linear-state marker carried on GraphDocumentDto::tags. A document with
    /// this tag projects to kind == FormSequence; without it, kind == Graph.
    inline constexpr std::string_view kLinearTag = "formsequence";

    /// One canvas node in the public contract. componentGuid is sourced from
    /// the internal target.component_ref.component_guid; dynamic_ports and
    /// target internals are dropped.
    struct GraphNode
    {
        std::string   id;
        std::string   component_guid;
        yyjson::value settings;
    };

    /// One canvas connection in the public contract. edge_id and edge_type are
    /// runtime-private and intentionally absent.
    struct GraphConnection
    {
        std::string from_node_id;
        std::string from_port_id;
        std::string to_node_id;
        std::string to_port_id;
    };

    /// One step in a linear formSequence view. `type` is the component kind
    /// (component_guid / entry kind) — a UI-facing label, not a runtime field.
    /// (Nested `children` from the schema draft is intentionally omitted for
    /// the flat linear projection; it can be added when nesting is needed.)
    struct SequenceItem
    {
        std::string   id;
        std::string   type;
        yyjson::value settings;
    };

    /// The formSequence projection of a linear GraphDocumentDto.
    struct FormSequenceView
    {
        std::vector<SequenceItem> sequence;
    };

    /// The canvas projection of a GraphDocumentDto.
    struct GraphView
    {
        std::vector<GraphNode>      nodes;
        std::vector<GraphConnection> connections;
    };

    enum class Kind
    {
        FormSequence,
        Graph,
    };

    /// The public authoring document. `kind` decides which of form_sequence /
    /// graph is populated.
    struct AuthoringDocument
    {
        int32_t                          contract_version = 1;
        Kind                             kind = Kind::Graph;
        int32_t                          revision = 0;
        std::string                      source_fingerprint;
        std::optional<FormSequenceView>  form_sequence;
        std::optional<GraphView>         graph;
    };

    // -----------------------------------------------------------------------
    // Projection: GraphDocumentDto (internal, authoritative) -> contract
    // -----------------------------------------------------------------------

    /// True when the document carries the linear marker tag.
    [[nodiscard]]
    bool IsLinear(const Dto::GraphDocumentDto& document) noexcept;

    /// Project the authoritative store into the public contract.
    ///   - Linear (tagged)  → kind == FormSequence; reverse-projects the linear
    ///                         signal chain into a flat SequenceItem[].
    ///   - Canvas (untagged)→ kind == Graph; thin-maps nodes/connections,
    ///                         dropping runtime-private fields.
    AuthoringDocument ToAuthoringDocument(const Dto::GraphDocumentDto& document);

    // -----------------------------------------------------------------------
    // Upgrade (formSequence → graph) — lossless, in-place on the store
    // -----------------------------------------------------------------------

    /// Remove the linear marker tag so the document projects to kind == Graph.
    /// The store is otherwise untouched → the upgrade is lossless and
    /// reversible (re-adding the tag restores the formSequence view). Returns
    /// true if the tag was present and removed.
    bool UpgradeToGraph(Dto::GraphDocumentDto& document) noexcept;

    /// Re-add the linear marker tag (graph → formSequence downgrade). Returns
    /// true if the tag was absent and added.
    bool DowngradeToFormSequence(Dto::GraphDocumentDto& document) noexcept;

    // -----------------------------------------------------------------------
    // Serialization (contract -> yyjson), for the ABI boundary to wrap as JSON
    // -----------------------------------------------------------------------

    /// Serialize the authoritative store into the public contract JSON value
    /// (camelCase keys, per the schema draft). Maps the store directly into a
    /// serializable shape, then builds the yyjson document with
    /// `yyjson::copy_string` so every string payload (id, componentGuid, …) is
    /// deep-copied into the document arena. The returned yyjson::value is
    /// therefore self-contained — it owns its document and references no
    /// outside storage — and is safe to return across the boundary / hand to
    /// the caller for writing. (Without copy_string yyjson would point at the
    /// std::string buffers of a local that dies on return → use-after-free.)
    yyjson::value SerializeDocument(const Dto::GraphDocumentDto& document);

    // -----------------------------------------------------------------------
    // Authoring change — dual-state dispatch on the authoritative store
    // -----------------------------------------------------------------------
    //
    // Mirrors the schema-draft AuthoringChange (`op` + payload fields). The
    // store's current mode decides how the change is applied:
    //
    //   - Graph mode (no linear tag): graph ops (addNode / removeNode /
    //     connectPorts / disconnectPorts / updateNodeConfig) are delegated to
    //     the GraphAuthoring engine (ApplySettingsChange) — the previously
    //     dormant authoring library is enabled, NOT rewritten.
    //   - Linear mode (formsequence tag): sequence ops (addSequenceItem /
    //     moveSequenceItem / removeSequenceItem / setValue) are reverse-
    //     projected onto a FormSequenceDto, applied via FormSequenceProjector
    //     (reusing its tested mutation logic), then re-projected back onto the
    //     GraphDocumentDto store (signal chain rebuilt). The store identity
    //     (document_id / fingerprint / tags) is preserved; revision bumps on
    //     success.
    //
    // `setValue` works in both modes (updates a node's settings by node_id).
    // On failure the store is left unmodified.
    // -----------------------------------------------------------------------

    struct AuthoringChange
    {
        std::string op; // schema op name (see AuthoringChange.ops in the schema)
        // addNode (graph)
        std::optional<GraphNode> node;
        // removeNode / updateNodeConfig / setValue / removeSequenceItem — id
        std::string node_id;
        // connectPorts (graph) — full connection; disconnectPorts — endpoints
        std::optional<GraphConnection> connection;
        // updateNodeConfig / setValue — new settings
        yyjson::value settings;
        // addSequenceItem / insertSequenceItem
        std::optional<SequenceItem> item;
        // moveSequenceItem
        std::optional<std::size_t> from;
        std::optional<std::size_t> to;
    };

    enum class ChangeErrorKind
    {
        None,
        InvalidOp,
        NodeNotFound,
        DuplicateNodeId,
        EmptyNodeId,
        InvalidEdge,
        EdgeNotFound,
        ItemNotFound,
        InvalidIndex,
        NoChange,
    };

    struct AuthoringChangeResult
    {
        ChangeErrorKind error_kind = ChangeErrorKind::None;
        std::string    message;

        [[nodiscard]]
        bool Ok() const noexcept
        {
            return error_kind == ChangeErrorKind::None;
        }
    };

    /// Apply a contract change to the authoritative store in place. On success
    /// bumps `document.version` and returns {None, ""}; on failure leaves the
    /// store unmodified and returns the error.
    AuthoringChangeResult ApplyAuthoringChange(
        Dto::GraphDocumentDto&   document,
        const AuthoringChange&   change);
} // namespace Contract

DAS_CORE_GRAPHRUNTIME_NS_END

// ---------------------------------------------------------------------------
// yyjson (de)serialization support for the contract value types
// ---------------------------------------------------------------------------

#ifndef DAS_GRAPHRUNTIME_CONTRACT_CASTER
#define DAS_GRAPHRUNTIME_CONTRACT_CASTER(DtoType)                              \
    template <>                                                                \
    struct yyjson::caster<Das::Core::GraphRuntime::Contract::DtoType>          \
    {                                                                          \
        template <yyjson::json_value Json>                                     \
        static Das::Core::GraphRuntime::Contract::DtoType from_json(           \
            const Json& json)                                                  \
        {                                                                      \
            auto object = json.as_object();                                    \
            if (!object.has_value())                                           \
            {                                                                  \
                throw yyjson::bad_cast(                                        \
                    "Contract type is not constructible from non-object JSON");\
            }                                                                  \
            return yyjson::detail::default_caster<                             \
                Das::Core::GraphRuntime::Contract::DtoType>::from_json(*object);\
        }                                                                      \
    }
#endif

DAS_GRAPHRUNTIME_CONTRACT_CASTER(GraphNode);
DAS_GRAPHRUNTIME_CONTRACT_CASTER(GraphConnection);
DAS_GRAPHRUNTIME_CONTRACT_CASTER(FormSequenceView);
DAS_GRAPHRUNTIME_CONTRACT_CASTER(GraphView);

#undef DAS_GRAPHRUNTIME_CONTRACT_CASTER

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Contract::GraphNode>
{
    using type = yyjson::snake_to_camel_transform;
};
template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Contract::GraphConnection>
{
    using type = yyjson::snake_to_camel_transform;
};
template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Contract::SequenceItem>
{
    using type = yyjson::snake_to_camel_transform;
};
template <>
struct yyjson::field_name_rule<
    Das::Core::GraphRuntime::Contract::FormSequenceView>
{
    using type = yyjson::snake_to_camel_transform;
};
template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Contract::GraphView>
{
    using type = yyjson::snake_to_camel_transform;
};

#endif // DAS_CORE_GRAPHRUNTIME_AUTHORINGDOCCONTRACT_H

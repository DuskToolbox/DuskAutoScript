#ifndef DAS_CORE_GRAPHRUNTIME_COMPILEDARTIFACT_H
#define DAS_CORE_GRAPHRUNTIME_COMPILEDARTIFACT_H

#include <cassert>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Utils/DasJsonCore.h>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

namespace Dto
{

    // ---- explicit two-state discriminator of a value-flow binding ----
    // kind is the only thing the runtime interprets to route a record:
    // Edge bindings copy a frame value from a producer site, Boundary
    // bindings materialise the graph's boundary input for a dangling
    // declared port (typed caller value first, declared default
    // otherwise). The record is self-describing — no sentinel node id
    // is part of the contract.
    enum class PortBindingKind
    {
        Edge,
        Boundary
    };

    // ---- data-flow binding between two ports ----
    struct PortBindingDto
    {
        PortBindingKind kind = PortBindingKind::Edge;
        // Edge state only — Boundary records carry no producer site (the
        // boundary port id IS the target port id under by-name merge).
        std::string source_node_id;
        std::string source_port_id;
        std::string target_node_id;
        std::string target_port_id;
        std::string expected_type;
        // Declared type of the target input port — the read-side
        // materialisation knowledge (integer sources widen into float
        // targets at consumer read time; float sources never truncate
        // into integer targets). Empty when unresolvable: the binding
        // plan never invents a type, mirroring expected_type's
        // source-side contract. Carried by both states — the Boundary
        // default materialisation needs the target-side type.
        std::string target_type;
        // Boundary state only — the boundary input's declared default so
        // the runtime can materialise the port even without an explicit
        // caller value. Empty for Edge bindings.
        yyjson::value default_value;
    };

    // ---- full routing table for a compiled graph ----
    struct PortBindingPlanDto
    {
        std::vector<PortBindingDto> bindings;
    };

    // ---- compiled snapshot of a single node ----
    struct CompiledNodeSnapshotDto
    {
        std::string                         node_id;
        std::string                         component_guid;
        yyjson::value                       compiled_payload_json;
        yyjson::value                       compiled_settings;
        std::vector<GraphPortDefinitionDto> resolved_ports;
    };

    // ---- signal activation route (DAS-60 signal-gated execution) ----
    // Describes a signal edge that activates a target port when the
    // activation_condition holds. Only the DTO shape + serialization is
    // defined here; population happens in a later stage.
    struct SignalRouteDto
    {
        std::string source_node_id;
        std::string source_port_id;
        std::string target_node_id;
        std::string target_port_id;
        // 源端是规范 signal_out 口（未声明、或声明为 signal 类型）且其成
        // 功发射由运行时兜底保证——编译器打标，运行时在组件未自发时补发。
        // 缺键容忍默认 false：旧计划不兜底，行为回落组件自发。
        bool          source_port_canonical = false;
        yyjson::value activation_condition;
    };

    // ---- controlled back edge (DAS-60 signal-gated loop control) ----
    // A back edge that closes a loop, plus the loop-head node it returns to.
    // edge_kind splits the two loop-carried channels explicitly: "signal"
    // back edges carry the iteration boundary (the runtime fires and re-pends
    // the loop on them), "data" back edges carry loop state (values survive
    // across iterations; they never fire).
    struct BackEdgeDto
    {
        std::string edge_id;
        std::string source_node_id;
        std::string source_port_id;
        std::string target_node_id;
        std::string target_port_id;
        std::string loop_head_node_id;
        std::string edge_kind = "signal";
    };

    // ---- port coordinate (node + port pair) ----
    // Used by loop-layer records that address a concrete port site (e.g. the
    // sources of data back edges feeding a loop layer).
    struct PortRefDto
    {
        std::string node_id;
        std::string port_id;
    };

    // ---- one loop layer of the compiled graph ----
    // A cycle layer is a strongly connected component that is an actual cycle
    // (size > 1, or a self-loop), optionally expanded with signal-less data
    // producers whose evaluation timing follows an in-loop consumer. tier is
    // the nesting depth (0 = outermost); the runtime consumes members as the
    // per-head iteration reset set and data_back_edge_sources as the stale
    // state channel of the loop.
    struct LoopLayerDto
    {
        std::string              head_node_id;
        int32_t                  tier = 0;
        std::vector<std::string> members;
        std::vector<PortRefDto>  data_back_edge_sources;
    };

    // ---- derived boundary port (ports-are-derived, never authored) ----
    // One boundary-facing port of a graph: an input port with no incoming
    // edge projects as a boundary input (the "value needs a source" minimum
    // set); every declared output port projects as a boundary output
    // (internal consumption and external export are not mutually exclusive).
    // Same-name sites merge under one boundary port (by-name broadcast
    // semantics) and `node_ids` carries every location so both the broadcast
    // binding generation and the runtime frame export can address them.
    struct BoundaryPortDto
    {
        std::string              port_id;
        std::string              display_label;
        std::string              port_type;
        bool                     is_required = false;
        yyjson::value            default_value;
        std::vector<std::string> node_ids;
    };

    struct BoundaryPortsDto
    {
        std::vector<BoundaryPortDto> inputs;
        std::vector<BoundaryPortDto> outputs;
    };

    // ---- bound repository reference recorded at compile time ----
    // One node bound to a repository entry (the compiler's business-neutral
    // taskRepositoryRef judgment). The runtime consumes the list verbatim as
    // the execution-time compile driver — it never re-derives the binding
    // from catalogs or repository stores.
    struct ReferenceEntryDto
    {
        std::string node_id;
        int64_t     entry_id = 0;
    };

    // ---- compiled plan for an entire graph ----
    struct CompiledGraphPlanDto
    {
        std::string                          document_id;
        int32_t                              source_revision = 0;
        std::string                          source_fingerprint;
        std::vector<CompiledNodeSnapshotDto> node_snapshots;
        PortBindingPlanDto                   binding_plan;
        std::vector<std::string>             execution_order;
        std::vector<yyjson::value>           diagnostics;
        std::vector<SignalRouteDto>          signal_routes;
        std::vector<BackEdgeDto>             back_edges;
        // Cycle layers (SCC ∪ signal-less producer closure) recorded at
        // compile time; the runtime consumes them as the per-head iteration
        // reset sets instead of recomputing reachability.
        std::vector<LoopLayerDto> loop_layers;
        // Derived boundary surface (ports-are-derived). Inputs (dangling
        // declared inputs) ride the Boundary binding generation; outputs
        // (all declared outputs) are the runtime's graphOutputs export
        // list.
        BoundaryPortsDto boundary_ports;
        // Bound-entry record (compile-time knowledge the runtime consumes
        // verbatim): the execution-time compile driver.
        std::vector<ReferenceEntryDto> reference_entries;
        // Closure stack of this graph (ancestor entry ids + this graph's own
        // entry id; empty for a top-level task graph). Rides the plan so the
        // execution-time compile hands each child session its inherited
        // closure — entry-cycle protection spans execution layers.
        std::vector<int64_t> closure_entry_ids;
    };

    // ---- compiled snapshot of a subgraph reference (02-07 serialization) ----
    struct CompiledSubgraphSnapshotDto
    {
        std::string                        graph_ref_id;
        int32_t                            graph_ref_revision = 0;
        std::string                        graph_ref_fingerprint;
        std::map<std::string, std::string> input_mapping;
        std::map<std::string, std::string> output_mapping;
        CompiledGraphPlanDto               inner_snapshot;
    };

} // namespace Dto

DAS_CORE_GRAPHRUNTIME_NS_END

// Macro-based caster: provides from_json via Detail::CastDtoObject.
// Re-defined here because GraphDocument.h #undef'd it after its own usage.
#define DAS_GRAPHRUNTIME_DTO_CASTER(DtoType)                                   \
    template <>                                                                \
    struct yyjson::caster<Das::Core::GraphRuntime::Dto::DtoType>               \
    {                                                                          \
        template <yyjson::json_value Json>                                     \
        static Das::Core::GraphRuntime::Dto::DtoType from_json(                \
            const Json& json)                                                  \
        {                                                                      \
            return Das::Core::GraphRuntime::Dto::Detail::CastDtoObject<        \
                Das::Core::GraphRuntime::Dto::DtoType>(json);                  \
        }                                                                      \
    }

DAS_GRAPHRUNTIME_DTO_CASTER(PortBindingDto);
DAS_GRAPHRUNTIME_DTO_CASTER(PortBindingPlanDto);
DAS_GRAPHRUNTIME_DTO_CASTER(CompiledNodeSnapshotDto);
DAS_GRAPHRUNTIME_DTO_CASTER(SignalRouteDto);
DAS_GRAPHRUNTIME_DTO_CASTER(BackEdgeDto);
DAS_GRAPHRUNTIME_DTO_CASTER(PortRefDto);
DAS_GRAPHRUNTIME_DTO_CASTER(LoopLayerDto);
DAS_GRAPHRUNTIME_DTO_CASTER(BoundaryPortDto);
DAS_GRAPHRUNTIME_DTO_CASTER(BoundaryPortsDto);
DAS_GRAPHRUNTIME_DTO_CASTER(ReferenceEntryDto);
DAS_GRAPHRUNTIME_DTO_CASTER(CompiledGraphPlanDto);
DAS_GRAPHRUNTIME_DTO_CASTER(CompiledSubgraphSnapshotDto);

#undef DAS_GRAPHRUNTIME_DTO_CASTER

// ---------------------------------------------------------------------------
// Leaf caster for PortBindingKind
// ---------------------------------------------------------------------------
// cpp_yyjson's aggregate reflection has no enum branch, so the kind field
// needs this leaf caster on both directions to keep riding the plain
// DAS_GRAPHRUNTIME_DTO_CASTER macro above. Wire form is the lowercase state
// name — a fail-closed throw on anything else (a tampered plan must not
// silently parse into either state).
template <>
struct yyjson::caster<Das::Core::GraphRuntime::Dto::PortBindingKind>
{
    using Kind = Das::Core::GraphRuntime::Dto::PortBindingKind;

    template <yyjson::json_value Json>
    static Kind from_json(const Json& json)
    {
        const auto text = json.as_string();
        if (text.has_value() && *text == std::string_view("edge"))
        {
            return Kind::Edge;
        }
        if (text.has_value() && *text == std::string_view("boundary"))
        {
            return Kind::Boundary;
        }
        throw yyjson::bad_cast(
            "PortBindingKind must be the string \"edge\" or \"boundary\"");
    }

    static void to_json(yyjson::writer::value_ref& value, Kind kind)
    {
        // String literals have static storage duration — no copy_string
        // pairing needed.
        value = kind == Kind::Edge ? std::string_view("edge")
                                   : std::string_view("boundary");
    }
};

// ---------------------------------------------------------------------------
// field_name_rule registrations (snake_case -> camelCase)
// ---------------------------------------------------------------------------
template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::PortBindingDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::PortBindingPlanDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::GraphRuntime::Dto::CompiledNodeSnapshotDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::SignalRouteDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::BackEdgeDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::PortRefDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::LoopLayerDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::BoundaryPortDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::BoundaryPortsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::ReferenceEntryDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::GraphRuntime::Dto::CompiledGraphPlanDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::GraphRuntime::Dto::CompiledSubgraphSnapshotDto>
{
    using type = yyjson::snake_to_camel_transform;
};

#endif // DAS_CORE_GRAPHRUNTIME_COMPILEDARTIFACT_H

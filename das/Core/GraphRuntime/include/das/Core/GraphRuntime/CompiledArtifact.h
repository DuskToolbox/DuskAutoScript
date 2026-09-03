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

    // ---- data-flow binding between two ports ----
    struct PortBindingDto
    {
        std::string source_node_id;
        std::string source_port_id;
        std::string target_node_id;
        std::string target_port_id;
        std::string expected_type;
        // Broadcast (graph-input) bindings carry the graph input's default so
        // the runtime can materialise the port even without an explicit value.
        // Empty for ordinary point-to-point data edges. (DAS-60 Stage 2)
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
        std::string   source_node_id;
        std::string   source_port_id;
        std::string   target_node_id;
        std::string   target_port_id;
        yyjson::value activation_condition;
    };

    // ---- controlled back edge (DAS-60 signal-gated loop control) ----
    // A back edge that closes a loop, plus the loop-head node it returns to.
    // Only the DTO shape + serialization is defined here.
    struct BackEdgeDto
    {
        std::string edge_id;
        std::string source_node_id;
        std::string source_port_id;
        std::string target_node_id;
        std::string target_port_id;
        std::string loop_head_node_id;
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
        // Derived boundary surface (ports-are-derived). Inputs (dangling
        // declared inputs) ride the broadcast binding generation
        // ($graph_input sentinel); outputs (all declared outputs) are the
        // runtime's graphOutputs export list.
        BoundaryPortsDto boundary_ports;
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
DAS_GRAPHRUNTIME_DTO_CASTER(BoundaryPortDto);
DAS_GRAPHRUNTIME_DTO_CASTER(BoundaryPortsDto);
DAS_GRAPHRUNTIME_DTO_CASTER(CompiledGraphPlanDto);
DAS_GRAPHRUNTIME_DTO_CASTER(CompiledSubgraphSnapshotDto);

#undef DAS_GRAPHRUNTIME_DTO_CASTER

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

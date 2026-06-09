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
#include <das/Core/GraphRuntime/GraphEntryId.h>
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

    // ---- compiled plan for an entire graph ----
    struct CompiledGraphPlanDto
    {
        std::string                          document_id;
        int32_t                              source_revision = 0;
        std::string                          source_fingerprint;
        std::string                          compiled_fingerprint;
        std::vector<CompiledNodeSnapshotDto> node_snapshots;
        PortBindingPlanDto                   binding_plan;
        std::vector<std::string>             execution_order;
        std::vector<yyjson::value>           diagnostics;
    };

    // ---- port mapping across subgraph boundaries ----
    struct PortMappingDto
    {
        std::string outer_port_id; // port_id in the outer (calling) graph
        std::string inner_port_id; // port_id in the inner (referenced) graph
        std::string node_id;   // node_id in the inner graph owning this port
        std::string port_type; // expected Variant type from port definition
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

    // ---- compile-time result from SubgraphCompiler (02-21, in-memory only)
    // ---- Recursive structure — NOT serialised via yyjson aggregate
    // reflection. Holds port projection mappings (outer↔inner), nested subgraph
    // results, and cycle-detection diagnostics.
    struct SubgraphCompileResultDto
    {
        GraphEntryId entry_id = 0;       // the referenced entry
        int64_t      revision = 0;       // pinned revision
        std::string  source_fingerprint; // pinned fingerprint

        CompiledGraphPlanDto compiled_plan; // from GraphCompiler::Compile()

        std::vector<PortMappingDto> input_mapping;  // outer graph → inner root
        std::vector<PortMappingDto> output_mapping; // inner terminal → outer

        std::vector<SubgraphCompileResultDto>
            nested_snapshots; // recursive children

        std::vector<std::string> diagnostics; // warnings and errors
    };

} // namespace Dto

DAS_CORE_GRAPHRUNTIME_NS_END

// ---------------------------------------------------------------------------
// yyjson caster<yyjson::value> is already defined in GraphDocument.h
// (guarded by DAS_YYJSON_VALUE_CASTER_DEFINED).  Do not redefine here.
// ---------------------------------------------------------------------------

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
DAS_GRAPHRUNTIME_DTO_CASTER(CompiledGraphPlanDto);
DAS_GRAPHRUNTIME_DTO_CASTER(PortMappingDto);
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
struct yyjson::field_name_rule<
    Das::Core::GraphRuntime::Dto::CompiledGraphPlanDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::PortMappingDto>
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

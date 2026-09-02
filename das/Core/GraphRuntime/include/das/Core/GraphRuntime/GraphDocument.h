#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHDOCUMENT_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHDOCUMENT_H

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Utils/DasJsonCore.h>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

namespace Dto
{
    struct GraphPortDefinitionDto
    {
        std::string              port_id;
        std::string              display_label;
        std::string              port_type;
        bool                     is_required = false;
        yyjson::value            default_value;
        std::vector<std::string> tags;
    };

    struct ComponentRefDto
    {
        std::string kind = "componentRef";
        std::string component_guid;
        std::string plugin_guid;
    };

    struct GraphNodeTargetDto
    {
        std::string                    target_kind;
        std::optional<ComponentRefDto> component_ref;
    };

    struct GraphNodeDto
    {
        std::string                         node_id;
        GraphNodeTargetDto                  target;
        yyjson::value                       settings;
        std::vector<GraphPortDefinitionDto> dynamic_ports;
        // User-authored display label. Pure presentation data — never a
        // node address, binding key or edge endpoint. Disengaged = never
        // renamed; the wire projection then falls back to the catalog
        // default name (componentGuid == actionId merge) before the client
        // side falls back to the node id.
        std::optional<std::string> label;
    };

    struct GraphEdgeDto
    {
        std::string edge_id;
        std::string source_node_id;
        std::string source_port_id;
        std::string target_node_id;
        std::string target_port_id;
        // "data" | "signal" — default "data" so pre-existing graphs keep
        // behaving as pure data-flow edges (DAS-60 signal-gated execution).
        std::string edge_type = "data";
    };

    struct GraphDocumentDto
    {
        std::string                         document_id;
        int32_t                             version = 0;
        std::string                         fingerprint;
        std::vector<GraphNodeDto>           nodes;
        std::vector<GraphEdgeDto>           edges;
        std::vector<GraphPortDefinitionDto> graph_inputs;
        std::vector<GraphPortDefinitionDto> graph_outputs;
        // Graph-level markers (e.g. "fromsequence") mirroring the per-port
        // GraphPortDefinitionDto::tags style.
        std::vector<std::string> tags;
        // UI-owned view data partition (e.g. node layout coordinates).
        // apply/compile pass it through without interpretation; the wire key
        // is "viewData" via the registered snake_to_camel rule.
        yyjson::value view_data;
    };

    namespace Detail
    {
        template <typename Dto, yyjson::json_value Json>
        Dto CastDtoObject(const Json& json)
        {
            auto object = json.as_object();
            if (!object.has_value())
            {
                throw yyjson::bad_cast(
                    "GraphRuntime DTO is not constructible from non-object "
                    "JSON");
            }

            return yyjson::detail::default_caster<Dto>::from_json(*object);
        }
    } // namespace Detail
} // namespace Dto

DAS_CORE_GRAPHRUNTIME_NS_END

// Macro-based caster: provides from_json via Detail::CastDtoObject.
// GraphNodeTargetDto is handled separately below because its
// std::optional<ComponentRefDto> field needs manual deserialization.
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

DAS_GRAPHRUNTIME_DTO_CASTER(GraphPortDefinitionDto);
DAS_GRAPHRUNTIME_DTO_CASTER(ComponentRefDto);
// GraphNodeTargetDto, GraphNodeDto and GraphDocumentDto omitted — custom
// casters below.
DAS_GRAPHRUNTIME_DTO_CASTER(GraphEdgeDto);

#undef DAS_GRAPHRUNTIME_DTO_CASTER

// ---------------------------------------------------------------------------
// Custom caster for GraphNodeTargetDto
// ---------------------------------------------------------------------------
// yyjson's aggregate default_caster cannot automatically deserialise
// std::optional<CustomDto> fields because the generic optional caster does
// not invoke our DAS_GRAPHRUNTIME_DTO_CASTER specialisations.
// We therefore provide a hand-written from_json that reads the camelCase
// keys and constructs the optionals explicitly.  Serialization (to_json) is
// still handled by yyjson's aggregate reflection + field_name_rule.
// ---------------------------------------------------------------------------
template <>
struct yyjson::caster<Das::Core::GraphRuntime::Dto::GraphNodeTargetDto>
{
    using Dto = Das::Core::GraphRuntime::Dto::GraphNodeTargetDto;

    template <yyjson::json_value Json>
    static Dto from_json(const Json& json)
    {
        auto object = json.as_object();
        if (!object.has_value())
        {
            throw yyjson::bad_cast(
                "GraphNodeTargetDto is not constructible from non-object JSON");
        }

        Dto target;

        if (auto v = (*object)[std::string_view("targetKind")].as_string())
        {
            target.target_kind = std::string(*v);
        }

        if (object->contains(std::string_view("componentRef")))
        {
            auto ref = (*object)[std::string_view("componentRef")];
            if (!ref.is_null())
            {
                target.component_ref =
                    Das::Core::GraphRuntime::Dto::Detail::CastDtoObject<
                        Das::Core::GraphRuntime::Dto::ComponentRefDto>(ref);
            }
        }

        return target;
    }
};

// ---------------------------------------------------------------------------
// Custom caster for GraphNodeDto
// ---------------------------------------------------------------------------
// Same rationale as GraphDocumentDto below: the aggregate default_caster
// resolves every field key through the strict object operator[], which
// throws out_of_range when the key is absent — a node persisted before the
// label field existed would fail to deserialize once label joined the
// aggregate. Guards every read with contains() so absent keys keep the
// field's default value. Serialization (to_json) stays on the aggregate
// reflection + field_name_rule path, matching the split above.
// ---------------------------------------------------------------------------
template <>
struct yyjson::caster<Das::Core::GraphRuntime::Dto::GraphNodeDto>
{
    using Dto = Das::Core::GraphRuntime::Dto::GraphNodeDto;

    template <yyjson::json_value Json>
    static Dto from_json(const Json& json)
    {
        auto object = json.as_object();
        if (!object.has_value())
        {
            throw yyjson::bad_cast(
                "GraphNodeDto is not constructible from non-object JSON");
        }

        Dto node;

        if (object->contains(std::string_view("nodeId")))
        {
            if (auto v = (*object)[std::string_view("nodeId")].as_string())
            {
                node.node_id = std::string(*v);
            }
        }
        if (object->contains(std::string_view("target")))
        {
            node.target =
                yyjson::cast<Das::Core::GraphRuntime::Dto::GraphNodeTargetDto>(
                    (*object)[std::string_view("target")]);
        }
        if (object->contains(std::string_view("settings")))
        {
            node.settings = Das::Utils::CloneYyjsonValue(
                (*object)[std::string_view("settings")]);
        }
        if (object->contains(std::string_view("dynamicPorts")))
        {
            node.dynamic_ports = yyjson::cast<std::vector<
                Das::Core::GraphRuntime::Dto::GraphPortDefinitionDto>>(
                (*object)[std::string_view("dynamicPorts")]);
        }
        if (object->contains(std::string_view("label")))
        {
            const auto& label_value = (*object)[std::string_view("label")];
            if (!label_value.is_null())
            {
                if (auto label = label_value.as_string())
                {
                    node.label = std::string(*label);
                }
            }
        }

        return node;
    }
};

// ---------------------------------------------------------------------------
// Custom caster for GraphDocumentDto
// ---------------------------------------------------------------------------
// The aggregate default_caster resolves every field key through the strict
// object operator[], which throws out_of_range when the key is absent — a
// document persisted before the viewData partition existed would therefore
// fail to deserialize once the view_data field joined the aggregate. This
// hand-written from_json guards every read with contains() so absent keys
// (including "viewData") keep the field's default value instead.
// Serialization (to_json) stays on the aggregate reflection + field_name_rule
// path, matching the GraphNodeTargetDto split above.
// ---------------------------------------------------------------------------
template <>
struct yyjson::caster<Das::Core::GraphRuntime::Dto::GraphDocumentDto>
{
    template <yyjson::json_value Json>
    static Das::Core::GraphRuntime::Dto::GraphDocumentDto from_json(
        const Json& json)
    {
        auto object = json.as_object();
        if (!object.has_value())
        {
            throw yyjson::bad_cast(
                "GraphDocumentDto is not constructible from non-object JSON");
        }

        Das::Core::GraphRuntime::Dto::GraphDocumentDto document;

        if (object->contains(std::string_view("documentId")))
        {
            if (auto v = (*object)[std::string_view("documentId")].as_string())
            {
                document.document_id = std::string(*v);
            }
        }
        if (object->contains(std::string_view("version")))
        {
            if (auto v = (*object)[std::string_view("version")].as_int())
            {
                document.version = static_cast<int32_t>(*v);
            }
        }
        if (object->contains(std::string_view("fingerprint")))
        {
            if (auto v = (*object)[std::string_view("fingerprint")].as_string())
            {
                document.fingerprint = std::string(*v);
            }
        }
        if (object->contains(std::string_view("nodes")))
        {
            document.nodes = yyjson::cast<
                std::vector<Das::Core::GraphRuntime::Dto::GraphNodeDto>>(
                (*object)[std::string_view("nodes")]);
        }
        if (object->contains(std::string_view("edges")))
        {
            document.edges = yyjson::cast<
                std::vector<Das::Core::GraphRuntime::Dto::GraphEdgeDto>>(
                (*object)[std::string_view("edges")]);
        }
        if (object->contains(std::string_view("graphInputs")))
        {
            document.graph_inputs = yyjson::cast<std::vector<
                Das::Core::GraphRuntime::Dto::GraphPortDefinitionDto>>(
                (*object)[std::string_view("graphInputs")]);
        }
        if (object->contains(std::string_view("graphOutputs")))
        {
            document.graph_outputs = yyjson::cast<std::vector<
                Das::Core::GraphRuntime::Dto::GraphPortDefinitionDto>>(
                (*object)[std::string_view("graphOutputs")]);
        }
        if (object->contains(std::string_view("tags")))
        {
            document.tags = yyjson::cast<std::vector<std::string>>(
                (*object)[std::string_view("tags")]);
        }
        if (object->contains(std::string_view("viewData")))
        {
            document.view_data = Das::Utils::CloneYyjsonValue(
                (*object)[std::string_view("viewData")]);
        }

        return document;
    }
};

// ---------------------------------------------------------------------------
// field_name_rule registrations (snake_case → camelCase)
// ---------------------------------------------------------------------------
template <>
struct yyjson::field_name_rule<
    Das::Core::GraphRuntime::Dto::GraphPortDefinitionDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::ComponentRefDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::GraphNodeTargetDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::GraphNodeDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::GraphEdgeDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::GraphDocumentDto>
{
    using type = yyjson::snake_to_camel_transform;
};

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHDOCUMENT_H

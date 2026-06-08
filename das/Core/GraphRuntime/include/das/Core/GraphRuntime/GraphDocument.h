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

    struct EntryRefDto
    {
        std::string                kind = "entryRef";
        int64_t                    entry_id = 0;
        std::optional<int64_t>     expected_revision;
        std::optional<std::string> source_fingerprint;
    };

    struct GraphNodeTargetDto
    {
        std::string                    target_kind;
        std::optional<ComponentRefDto> component_ref;
        std::optional<EntryRefDto>     entry_ref;
    };

    struct GraphNodeDto
    {
        std::string                         node_id;
        GraphNodeTargetDto                  target;
        yyjson::value                       settings;
        std::vector<GraphPortDefinitionDto> dynamic_ports;
    };

    struct GraphEdgeDto
    {
        std::string edge_id;
        std::string source_node_id;
        std::string source_port_id;
        std::string target_node_id;
        std::string target_port_id;
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

// Guard against duplicate definition if TaskRepositoryDtos.h is included first.
#ifndef DAS_YYJSON_VALUE_CASTER_DEFINED
#define DAS_YYJSON_VALUE_CASTER_DEFINED

template <>
struct yyjson::caster<yyjson::value>
{
    template <yyjson::json_value Json>
    static yyjson::value from_json(const Json& json)
    {
        return Das::Utils::CloneYyjsonValue(json);
    }
};

#endif // DAS_YYJSON_VALUE_CASTER_DEFINED

// Macro-based caster: provides from_json via Detail::CastDtoObject.
// GraphNodeTargetDto is handled separately below because its
// std::optional<ComponentRefDto> / std::optional<EntryRefDto> fields need
// manual deserialization.
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
DAS_GRAPHRUNTIME_DTO_CASTER(EntryRefDto);
// GraphNodeTargetDto omitted — custom caster below.
DAS_GRAPHRUNTIME_DTO_CASTER(GraphNodeDto);
DAS_GRAPHRUNTIME_DTO_CASTER(GraphEdgeDto);
DAS_GRAPHRUNTIME_DTO_CASTER(GraphDocumentDto);

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

        if (object->contains(std::string_view("entryRef")))
        {
            auto ref = (*object)[std::string_view("entryRef")];
            if (!ref.is_null())
            {
                target.entry_ref =
                    Das::Core::GraphRuntime::Dto::Detail::CastDtoObject<
                        Das::Core::GraphRuntime::Dto::EntryRefDto>(ref);
            }
        }

        return target;
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
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::EntryRefDto>
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

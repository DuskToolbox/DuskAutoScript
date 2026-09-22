#ifndef DAS_CORE_GRAPH_RUNTIME_REPOSITORY_INVOKE_DTOS_H
#define DAS_CORE_GRAPH_RUNTIME_REPOSITORY_INVOKE_DTOS_H

#include <cassert>
#include <cpp_yyjson.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace Das::Core::GraphRuntime::RepositoryInvoke::Dto
{
    // Repository reference wire; home is GraphRuntime because the
    // reconciliation fields (expectedRevision / sourceFingerprint) ride with
    // the closure-driven compile chain. The invokeRepository component's
    // snapshot wire retired together with that component.
    struct RepositoryTaskRefDto
    {
        std::string                kind = "taskRepositoryRef";
        int64_t                    entry_id = 0;
        std::optional<int64_t>     expected_revision;
        std::optional<std::string> source_fingerprint;
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
                    "Repository invoke DTO is not constructible from "
                    "non-object JSON");
            }

            return yyjson::detail::default_caster<Dto>::from_json(*object);
        }
    } // namespace Detail
} // namespace Das::Core::GraphRuntime::RepositoryInvoke::Dto

#define DAS_GRAPH_RUNTIME_REPOSITORY_INVOKE_DTO_CASTER(DtoType)                \
    template <>                                                                \
    struct yyjson::caster<                                                     \
        Das::Core::GraphRuntime::RepositoryInvoke::Dto::DtoType>               \
    {                                                                          \
        template <yyjson::json_value Json>                                     \
        static Das::Core::GraphRuntime::RepositoryInvoke::Dto::DtoType         \
        from_json(const Json& json)                                            \
        {                                                                      \
            return Das::Core::GraphRuntime::RepositoryInvoke::Dto::Detail::    \
                CastDtoObject<                                                 \
                    Das::Core::GraphRuntime::RepositoryInvoke::Dto::DtoType>(  \
                    json);                                                     \
        }                                                                      \
    }

DAS_GRAPH_RUNTIME_REPOSITORY_INVOKE_DTO_CASTER(RepositoryTaskRefDto);

#undef DAS_GRAPH_RUNTIME_REPOSITORY_INVOKE_DTO_CASTER

template <>
struct yyjson::field_name_rule<
    Das::Core::GraphRuntime::RepositoryInvoke::Dto::RepositoryTaskRefDto>
{
    using type = yyjson::snake_to_camel_transform;
};

#endif // DAS_CORE_GRAPH_RUNTIME_REPOSITORY_INVOKE_DTOS_H

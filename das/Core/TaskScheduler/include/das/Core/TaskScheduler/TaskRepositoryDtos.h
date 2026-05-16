#ifndef DAS_CORE_TASK_SCHEDULER_TASK_REPOSITORY_DTOS_H
#define DAS_CORE_TASK_SCHEDULER_TASK_REPOSITORY_DTOS_H

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cpp_yyjson.hpp>

namespace Das::Core::TaskScheduler::Repository::Dto
{
    struct RepositoryAuthoringMetadataDto
    {
        int64_t                    revision = 0;
        std::string                kind;
        std::string                source_fingerprint;
        std::optional<std::string> migration_state;
    };

    struct RepositoryAvailabilityDto
    {
        std::string state;
        std::string reason;
        std::string message;
    };

    struct RepositoryEntryDto
    {
        int64_t                        entry_id = 0;
        std::string                    display_name;
        std::string                    plugin_guid;
        std::string                    task_type_guid;
        RepositoryAuthoringMetadataDto authoring;
        yyjson::value                  accepted_properties;
        RepositoryAvailabilityDto      availability;
    };

    struct CreateRepositoryEntryRequestDto
    {
        std::string                plugin_guid;
        std::string                task_type_guid;
        std::optional<std::string> display_name;
        yyjson::value              initial_properties;
    };

    struct RenameRepositoryEntryRequestDto
    {
        std::string display_name;
    };

    struct RepositoryEntryRefDto
    {
        std::string                kind = "taskRepositoryRef";
        int64_t                    entry_id = 0;
        std::optional<int64_t>     expected_revision;
        std::optional<std::string> source_fingerprint;
    };

    struct RepositoryGetResponseDto
    {
        std::vector<RepositoryEntryDto> entries;
    };

    struct RepositoryAuthoringResultDto
    {
        int64_t                        entry_id = 0;
        RepositoryAuthoringMetadataDto authoring;
        yyjson::value                  accepted_properties;
        yyjson::value                  document;
        std::vector<yyjson::value>     diagnostics;
    };

    struct RepositoryCompileSummaryDto
    {
        bool                     can_execute = false;
        std::vector<std::string> task_names;
        bool                     requires_agent_runtime = false;
    };

    struct RepositoryCompileResultDto
    {
        bool                        can_execute = false;
        RepositoryCompileSummaryDto summary;
        std::vector<yyjson::value>  diagnostics;
        yyjson::value               debug_compile;
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
                    "Repository DTO is not constructible from non-object JSON");
            }

            return yyjson::detail::default_caster<Dto>::from_json(*object);
        }
    } // namespace Detail
} // namespace Das::Core::TaskScheduler::Repository::Dto

template <>
struct yyjson::caster<yyjson::value>
{
    template <yyjson::json_value Json>
    static yyjson::value from_json(const Json& json)
    {
        auto serialized = json.write();
        return yyjson::read(
            std::string_view(serialized.data(), serialized.size()));
    }
};

#define DAS_TASK_REPOSITORY_DTO_CASTER(DtoType)                                \
    template <>                                                                \
    struct yyjson::caster<Das::Core::TaskScheduler::Repository::Dto::DtoType>  \
    {                                                                          \
        template <yyjson::json_value Json>                                     \
        static Das::Core::TaskScheduler::Repository::Dto::DtoType from_json(   \
            const Json& json)                                                  \
        {                                                                      \
            return Das::Core::TaskScheduler::Repository::Dto::Detail::         \
                CastDtoObject<                                                 \
                    Das::Core::TaskScheduler::Repository::Dto::DtoType>(json); \
        }                                                                      \
    }

DAS_TASK_REPOSITORY_DTO_CASTER(RepositoryAuthoringMetadataDto);
DAS_TASK_REPOSITORY_DTO_CASTER(RepositoryEntryDto);
DAS_TASK_REPOSITORY_DTO_CASTER(CreateRepositoryEntryRequestDto);
DAS_TASK_REPOSITORY_DTO_CASTER(RenameRepositoryEntryRequestDto);
DAS_TASK_REPOSITORY_DTO_CASTER(RepositoryEntryRefDto);
DAS_TASK_REPOSITORY_DTO_CASTER(RepositoryGetResponseDto);
DAS_TASK_REPOSITORY_DTO_CASTER(RepositoryAuthoringResultDto);
DAS_TASK_REPOSITORY_DTO_CASTER(RepositoryCompileSummaryDto);
DAS_TASK_REPOSITORY_DTO_CASTER(RepositoryCompileResultDto);

#undef DAS_TASK_REPOSITORY_DTO_CASTER

template <>
struct yyjson::caster<
    Das::Core::TaskScheduler::Repository::Dto::RepositoryAvailabilityDto>
{
    template <yyjson::json_value Json>
    static Das::Core::TaskScheduler::Repository::Dto::RepositoryAvailabilityDto
    from_json(const Json& json)
    {
        auto object = json.as_object();
        if (!object.has_value())
        {
            throw yyjson::bad_cast(
                "Repository availability is not constructible from "
                "non-object JSON");
        }

        auto string_or_empty =
            [&object](std::string_view key) -> std::string
        {
            if (!object->contains(key))
            {
                return {};
            }
            auto value = (*object)[key].as_string();
            if (!value)
            {
                return {};
            }
            return std::string(*value);
        };

        Das::Core::TaskScheduler::Repository::Dto::RepositoryAvailabilityDto
            availability;
        availability.state = string_or_empty("state");
        availability.reason = string_or_empty("reason");
        availability.message = string_or_empty("message");
        return availability;
    }
};

template <>
struct yyjson::field_name_rule<
    Das::Core::TaskScheduler::Repository::Dto::RepositoryAuthoringMetadataDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::TaskScheduler::Repository::Dto::RepositoryAvailabilityDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::TaskScheduler::Repository::Dto::CreateRepositoryEntryRequestDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::TaskScheduler::Repository::Dto::RenameRepositoryEntryRequestDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryRefDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::TaskScheduler::Repository::Dto::RepositoryGetResponseDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::TaskScheduler::Repository::Dto::RepositoryAuthoringResultDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::TaskScheduler::Repository::Dto::RepositoryCompileSummaryDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    Das::Core::TaskScheduler::Repository::Dto::RepositoryCompileResultDto>
{
    using type = yyjson::snake_to_camel_transform;
};

#endif // DAS_CORE_TASK_SCHEDULER_TASK_REPOSITORY_DTOS_H

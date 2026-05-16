#ifndef DAS_CORE_TASK_SCHEDULER_REPOSITORY_INVOKE_DTOS_H
#define DAS_CORE_TASK_SCHEDULER_REPOSITORY_INVOKE_DTOS_H

#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>

#include <cpp_yyjson.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Das::Core::TaskScheduler::RepositoryInvoke::Dto
{
    struct RepositoryTaskRefDto
    {
        std::string                kind = "taskRepositoryRef";
        int64_t                    entry_id = 0;
        std::optional<int64_t>     expected_revision;
        std::optional<std::string> source_fingerprint;
    };

    struct ChildExecutionSnapshotDto
    {
        int32_t                    version = 1;
        int64_t                    source_entry_id = 0;
        int64_t                    source_revision = 0;
        std::optional<std::string> source_fingerprint;
        std::string                plugin_guid;
        std::string                task_type_guid;
        std::string                component_guid;
        yyjson::value              execution_input;
    };

    struct InvokeRepositoryTaskSettingsDto
    {
        RepositoryTaskRefDto repository_ref;
        bool                 fail_on_revision_mismatch = true;
    };

    struct InvokeRepositoryTaskInputDto
    {
        std::optional<ChildExecutionSnapshotDto> compiled_snapshot;
        yyjson::value                            runtime_inputs;
    };

    struct InvokeRepositoryTaskDiagnosticDto
    {
        std::string                severity;
        std::string                code;
        std::string                message;
        std::optional<std::string> path;
    };

    struct InvokeRepositoryTaskOutputsDto
    {
        std::optional<std::string> child_status;
        yyjson::value              child_outputs;
    };

    struct InvokeRepositoryTaskSignalsDto
    {
        bool succeeded = false;
        bool failed = false;
        bool cancelled = false;
    };

    struct InvokeRepositoryTaskResultDto
    {
        int32_t                                      version = 1;
        std::string                                  status;
        InvokeRepositoryTaskOutputsDto                 outputs;
        std::vector<InvokeRepositoryTaskDiagnosticDto> diagnostics;
        InvokeRepositoryTaskSignalsDto                 signals;
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
} // namespace Das::Core::TaskScheduler::RepositoryInvoke::Dto

#define DAS_REPOSITORY_INVOKE_DTO_CASTER(DtoType)                             \
    template <>                                                                \
    struct yyjson::caster<                                                     \
        Das::Core::TaskScheduler::RepositoryInvoke::Dto::DtoType>              \
    {                                                                          \
        template <yyjson::json_value Json>                                     \
        static Das::Core::TaskScheduler::RepositoryInvoke::Dto::DtoType        \
        from_json(const Json& json)                                            \
        {                                                                      \
            return Das::Core::TaskScheduler::RepositoryInvoke::Dto::Detail::   \
                CastDtoObject<                                                 \
                    Das::Core::TaskScheduler::RepositoryInvoke::Dto::DtoType>( \
                    json);                                                     \
        }                                                                      \
    }

DAS_REPOSITORY_INVOKE_DTO_CASTER(RepositoryTaskRefDto);
DAS_REPOSITORY_INVOKE_DTO_CASTER(ChildExecutionSnapshotDto);
DAS_REPOSITORY_INVOKE_DTO_CASTER(InvokeRepositoryTaskSettingsDto);
DAS_REPOSITORY_INVOKE_DTO_CASTER(InvokeRepositoryTaskInputDto);
DAS_REPOSITORY_INVOKE_DTO_CASTER(InvokeRepositoryTaskDiagnosticDto);
DAS_REPOSITORY_INVOKE_DTO_CASTER(InvokeRepositoryTaskOutputsDto);
DAS_REPOSITORY_INVOKE_DTO_CASTER(InvokeRepositoryTaskSignalsDto);
DAS_REPOSITORY_INVOKE_DTO_CASTER(InvokeRepositoryTaskResultDto);

#undef DAS_REPOSITORY_INVOKE_DTO_CASTER

template <>
struct yyjson::field_name_rule<
    Das::Core::TaskScheduler::RepositoryInvoke::Dto::RepositoryTaskRefDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::TaskScheduler::RepositoryInvoke::Dto::
                                   ChildExecutionSnapshotDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::TaskScheduler::RepositoryInvoke::Dto::
                                   InvokeRepositoryTaskSettingsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::TaskScheduler::RepositoryInvoke::Dto::
                                   InvokeRepositoryTaskInputDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::TaskScheduler::RepositoryInvoke::Dto::
                                   InvokeRepositoryTaskDiagnosticDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::TaskScheduler::RepositoryInvoke::Dto::
                                   InvokeRepositoryTaskOutputsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::TaskScheduler::RepositoryInvoke::Dto::
                                   InvokeRepositoryTaskSignalsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::TaskScheduler::RepositoryInvoke::Dto::
                                   InvokeRepositoryTaskResultDto>
{
    using type = yyjson::snake_to_camel_transform;
};

#endif // DAS_CORE_TASK_SCHEDULER_REPOSITORY_INVOKE_DTOS_H

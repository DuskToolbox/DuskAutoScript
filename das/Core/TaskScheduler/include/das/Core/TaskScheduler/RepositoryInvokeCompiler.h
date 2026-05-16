#ifndef DAS_CORE_TASK_SCHEDULER_REPOSITORY_INVOKE_COMPILER_H
#define DAS_CORE_TASK_SCHEDULER_REPOSITORY_INVOKE_COMPILER_H

#include <das/Core/TaskScheduler/RepositoryInvokeDtos.h>
#include <das/Core/TaskScheduler/RepositoryInvokeGraph.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>

#include <cpp_yyjson.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Das::Core::TaskScheduler::RepositoryInvoke
{
    struct RepositoryInvokeCompileDiagnostic
    {
        std::string severity = "error";
        std::string code;
        std::string message;
        std::optional<std::string> path;
        std::optional<int64_t>     current_revision;
        std::optional<int64_t>     expected_revision;
        std::optional<std::string> current_source_fingerprint;
        std::optional<std::string> expected_source_fingerprint;
        std::vector<RepositoryCyclePathItem> cycle_path;
    };

    struct RepositoryInvokeProviderCompileResult
    {
        bool                                      ok = false;
        yyjson::value                            compile_result;
        std::vector<RepositoryInvokeCompileDiagnostic> diagnostics;
    };

    struct RepositoryInvokeCompileResult
    {
        bool ok = false;
        std::optional<Dto::ChildExecutionSnapshotDto> snapshot;
        std::vector<RepositoryInvokeCompileDiagnostic> diagnostics;
        std::vector<RepositoryCyclePathItem> cycle_path;
    };

    struct RepositoryInvokeCompileServices
    {
        std::function<std::optional<Repository::Dto::RepositoryEntryDto>(
            int64_t entry_id)>
            load_entry;
        std::function<std::optional<std::string>(
            const Repository::Dto::RepositoryEntryDto& entry)>
            find_execution_component;
        std::function<RepositoryInvokeProviderCompileResult(
            const Repository::Dto::RepositoryEntryDto& entry,
            const yyjson::value& request)>
            compile_authoring;
    };

    RepositoryInvokeCompileResult ResolveRepositoryInvokeSnapshot(
        const RepositoryInvokeCompileServices& services,
        const Dto::RepositoryTaskRefDto&       repository_ref);

} // namespace Das::Core::TaskScheduler::RepositoryInvoke

#endif // DAS_CORE_TASK_SCHEDULER_REPOSITORY_INVOKE_COMPILER_H

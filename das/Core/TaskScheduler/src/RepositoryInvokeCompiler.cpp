#include <das/Core/TaskScheduler/RepositoryInvokeCompiler.h>

#include <string_view>

namespace Das::Core::TaskScheduler::RepositoryInvoke
{
    namespace
    {
        RepositoryInvokeCompileResult Fail(
            RepositoryInvokeCompileDiagnostic diagnostic)
        {
            RepositoryInvokeCompileResult result;
            result.ok = false;
            result.diagnostics.push_back(std::move(diagnostic));
            result.cycle_path = result.diagnostics.front().cycle_path;
            return result;
        }

        RepositoryInvokeCompileDiagnostic MakeDiagnostic(
            std::string code,
            std::string message)
        {
            RepositoryInvokeCompileDiagnostic diagnostic;
            diagnostic.code = std::move(code);
            diagnostic.message = std::move(message);
            return diagnostic;
        }

        bool IsAvailable(
            const Repository::Dto::RepositoryAvailabilityDto& availability)
        {
            return availability.state.empty() || availability.state == "available";
        }
    } // namespace

    RepositoryInvokeCompileResult ResolveRepositoryInvokeSnapshot(
        const RepositoryInvokeCompileServices& services,
        const Dto::RepositoryTaskRefDto&       repository_ref)
    {
        if (!services.load_entry)
        {
            return Fail(MakeDiagnostic(
                "repository-loader-missing",
                "Repository invoke compiler was not given an entry loader"));
        }

        auto entry = services.load_entry(repository_ref.entry_id);
        if (!entry)
        {
            return Fail(MakeDiagnostic(
                "repository-entry-not-found",
                "Repository entry was not found"));
        }

        if (!IsAvailable(entry->availability))
        {
            return Fail(MakeDiagnostic(
                "repository-entry-unavailable",
                entry->availability.message.empty()
                    ? "Repository entry is unavailable"
                    : entry->availability.message));
        }

        if (repository_ref.expected_revision
            && *repository_ref.expected_revision != entry->authoring.revision)
        {
            auto diagnostic = MakeDiagnostic(
                "repository-revision-mismatch",
                "Repository entry revision did not match the expected revision");
            diagnostic.current_revision = entry->authoring.revision;
            diagnostic.expected_revision = repository_ref.expected_revision;
            return Fail(std::move(diagnostic));
        }

        if (repository_ref.source_fingerprint
            && *repository_ref.source_fingerprint
                   != entry->authoring.source_fingerprint)
        {
            auto diagnostic = MakeDiagnostic(
                "repository-source-fingerprint-mismatch",
                "Repository entry source fingerprint did not match");
            diagnostic.current_source_fingerprint =
                entry->authoring.source_fingerprint;
            diagnostic.expected_source_fingerprint =
                repository_ref.source_fingerprint;
            return Fail(std::move(diagnostic));
        }

        if (!services.find_execution_component)
        {
            return Fail(MakeDiagnostic(
                "execution-component-missing",
                "Repository entry task type does not declare an execution "
                "component"));
        }

        auto execution_component = services.find_execution_component(*entry);
        if (!execution_component || execution_component->empty())
        {
            return Fail(MakeDiagnostic(
                "execution-component-missing",
                "Repository entry task type does not declare an execution "
                "component"));
        }

        return Fail(MakeDiagnostic(
            "repository-execution-compile-unimplemented",
            "Repository execution compile is not implemented"));
    }
} // namespace Das::Core::TaskScheduler::RepositoryInvoke

#include <das/Core/TaskScheduler/RepositoryInvokeCompiler.h>

#include <das/Utils/DasJsonCore.h>

#include <cstdint>
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
            return availability.state.empty()
                   || availability.state == "available";
        }

        template <typename Json>
        yyjson::value CloneJsonValue(const Json& value)
        {
            return Das::Utils::CloneYyjsonValue(value);
        }

        yyjson::value MakeExecutionCompileRequest()
        {
            yyjson::value request(Das::Utils::MakeYyjsonObject());
            auto          obj = request.as_object();
            (*obj)[std::string_view("purpose")] = "execution";
            return request;
        }

        bool IsExplicitFalse(const yyjson::value& value)
        {
            auto bool_value = value.as_bool();
            return bool_value.has_value() && !*bool_value;
        }

        std::optional<yyjson::value> ExtractExecutionInput(
            const yyjson::value& compile_result)
        {
            auto obj = compile_result.as_object();
            if (!obj || !obj->contains(std::string_view("executionInput")))
            {
                return std::nullopt;
            }
            return CloneJsonValue((*obj)[std::string_view("executionInput")]);
        }

        RepositoryInvokeCompileResult Succeed(
            Dto::ChildExecutionSnapshotDto snapshot)
        {
            RepositoryInvokeCompileResult result;
            result.ok = true;
            result.snapshot = std::move(snapshot);
            return result;
        }

        template <typename ObjectRef>
        std::optional<int64_t> GetIntField(
            const ObjectRef& object,
            std::string_view key)
        {
            if (!object.contains(key))
            {
                return std::nullopt;
            }
            return object[key].as_sint();
        }

        template <typename ObjectRef>
        std::optional<std::string> GetStringField(
            const ObjectRef& object,
            std::string_view key)
        {
            if (!object.contains(key))
            {
                return std::nullopt;
            }
            auto value = object[key].as_string();
            if (!value)
            {
                return std::nullopt;
            }
            return std::string(*value);
        }

        template <typename ObjectRef>
        std::optional<std::string> FirstStringField(
            const ObjectRef& object,
            std::string_view first,
            std::string_view second)
        {
            if (auto value = GetStringField(object, first))
            {
                return value;
            }
            return GetStringField(object, second);
        }

        template <typename ObjectRef>
        void AddEdgeFromRepositoryRef(
            const ObjectRef&                       ref,
            int64_t                                source_entry_id,
            const std::optional<std::string>&      source_node_id,
            const std::optional<std::string>&      source_node_label,
            std::vector<RepositoryDependencyEdge>& edges)
        {
            auto kind = GetStringField(ref, "kind");
            if (kind && *kind != "taskRepositoryRef")
            {
                return;
            }

            auto target_entry_id = GetIntField(ref, "entryId");
            if (!target_entry_id)
            {
                return;
            }

            RepositoryDependencyEdge edge;
            edge.source_entry_id = source_entry_id;
            edge.target_entry_id = *target_entry_id;
            edge.source_node_id = source_node_id;
            edge.source_node_label = source_node_label;
            edges.push_back(std::move(edge));
        }

        template <typename JsonValue>
        void ExtractEdgesRecursive(
            int64_t                                inherited_source_entry_id,
            const JsonValue&                       value,
            std::optional<std::string>             inherited_node_id,
            std::optional<std::string>             inherited_node_label,
            std::vector<RepositoryDependencyEdge>& edges)
        {
            if (auto array = value.as_array())
            {
                for (const auto& item : *array)
                {
                    ExtractEdgesRecursive(
                        inherited_source_entry_id,
                        item,
                        inherited_node_id,
                        inherited_node_label,
                        edges);
                }
                return;
            }

            auto object = value.as_object();
            if (!object)
            {
                return;
            }

            auto source_entry_id = inherited_source_entry_id;
            if (auto source_override = GetIntField(*object, "sourceEntryId"))
            {
                source_entry_id = *source_override;
            }

            auto source_node_id = FirstStringField(*object, "id", "nodeId");
            if (!source_node_id)
            {
                source_node_id = inherited_node_id;
            }
            auto source_node_label = FirstStringField(*object, "label", "name");
            if (!source_node_label)
            {
                source_node_label = inherited_node_label;
            }

            for (auto key :
                 {std::string_view("taskRepositoryRef"),
                  std::string_view("repositoryRef")})
            {
                if (!object->contains(key))
                {
                    continue;
                }
                auto ref = (*object)[key].as_object();
                if (ref)
                {
                    AddEdgeFromRepositoryRef(
                        *ref,
                        source_entry_id,
                        source_node_id,
                        source_node_label,
                        edges);
                }
            }

            for (const auto& [_, child] : *object)
            {
                ExtractEdgesRecursive(
                    source_entry_id,
                    child,
                    source_node_id,
                    source_node_label,
                    edges);
            }
        }
    } // namespace

    RepositoryInvokeCompileResult ResolveRepositoryInvokeSnapshot(
        const RepositoryInvokeCompileServices& services,
        const Dto::RepositoryTaskRefDto&       repository_ref)
    {
        return ResolveRepositoryInvokeSnapshot(
            services,
            repository_ref,
            RepositoryInvokeSourceContext{});
    }

    RepositoryInvokeCompileResult ResolveRepositoryInvokeSnapshot(
        const RepositoryInvokeCompileServices& services,
        const Dto::RepositoryTaskRefDto&       repository_ref,
        const RepositoryInvokeSourceContext&   source_context)
    {
        if (!source_context.source_graph.is_null())
        {
            auto edges = ExtractRepositoryInvokeDependencyEdges(
                source_context.source_entry_id,
                source_context.source_graph);
            auto validation = ValidateRepositoryInvokeAcyclic(edges);
            if (!validation.ok)
            {
                auto diagnostic = MakeDiagnostic(
                    "repository-invoke-cycle",
                    "Repository invoke graph contains a cycle");
                diagnostic.cycle_path = validation.cycle_path;

                auto result = Fail(std::move(diagnostic));
                result.cycle_path = validation.cycle_path;
                return result;
            }
        }

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

        if (!services.compile_authoring)
        {
            return Fail(MakeDiagnostic(
                "provider-compile-failed",
                "Repository authoring provider compile is not available"));
        }

        auto provider_result =
            services.compile_authoring(*entry, MakeExecutionCompileRequest());
        auto       compile_obj = provider_result.compile_result.as_object();
        const bool provider_returned_false =
            compile_obj && compile_obj->contains(std::string_view("ok"))
            && IsExplicitFalse((*compile_obj)[std::string_view("ok")]);
        if (!provider_result.ok || provider_returned_false)
        {
            if (!provider_result.diagnostics.empty())
            {
                RepositoryInvokeCompileResult result;
                result.ok = false;
                result.diagnostics = provider_result.diagnostics;
                return result;
            }
            return Fail(MakeDiagnostic(
                "provider-compile-failed",
                "Repository authoring provider failed to compile execution "
                "input"));
        }

        auto execution_input =
            ExtractExecutionInput(provider_result.compile_result);
        if (!execution_input)
        {
            return Fail(MakeDiagnostic(
                "execution-input-missing",
                "Repository execution compile did not return executionInput"));
        }

        Dto::ChildExecutionSnapshotDto snapshot;
        snapshot.source_entry_id = entry->entry_id;
        snapshot.source_revision = entry->authoring.revision;
        if (!entry->authoring.source_fingerprint.empty())
        {
            snapshot.source_fingerprint = entry->authoring.source_fingerprint;
        }
        snapshot.plugin_guid = entry->plugin_guid;
        snapshot.task_type_guid = entry->task_type_guid;
        snapshot.component_guid = *execution_component;
        snapshot.execution_input = std::move(*execution_input);
        return Succeed(std::move(snapshot));
    }

    std::vector<RepositoryDependencyEdge>
    ExtractRepositoryInvokeDependencyEdges(
        int64_t              source_entry_id,
        const yyjson::value& source_graph)
    {
        std::vector<RepositoryDependencyEdge> edges;
        ExtractEdgesRecursive(
            source_entry_id,
            source_graph,
            std::nullopt,
            std::nullopt,
            edges);
        return edges;
    }
} // namespace Das::Core::TaskScheduler::RepositoryInvoke

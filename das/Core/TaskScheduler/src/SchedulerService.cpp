#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/Core/TaskScheduler/TaskAuthoringContract.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <ranges>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace Das::Core::TaskScheduler
{

    using Das::Core::ForeignInterfaceHost::FindManifest;
    using Das::Core::ForeignInterfaceHost::PluginPackageDesc;
    using Das::Core::ForeignInterfaceHost::PluginSettingDesc;
    using Das::Core::ForeignInterfaceHost::TaskDescriptor;

    namespace
    {
        using SystemClock = std::chrono::system_clock;

        // Sentinel far-future timestamp used when a task's
        // GetNextExecutionTime() fails. Prevents zero-delay hot loops by
        // pushing the next run far into the future.
        // 2099-12-31T23:59:59 UTC
        static constexpr int64_t SENTINEL_FUTURE_TIMESTAMP = 4102444799LL;

        time_t TimegmPortable(std::tm* tm)
        {
#ifdef _WIN32
            return _mkgmtime(tm);
#else
            return timegm(tm);
#endif
        }

        int64_t DasDateToUnix(const Das::ExportInterface::DasDate& date)
        {
            std::tm tm{};
            tm.tm_year = date.year - 1900;
            tm.tm_mon = date.month - 1;
            tm.tm_mday = date.day;
            tm.tm_hour = date.hour;
            tm.tm_min = date.minute;
            tm.tm_sec = date.second;
            tm.tm_isdst = -1;
            return static_cast<int64_t>(TimegmPortable(&tm));
        }

        std::chrono::steady_clock::duration ClampNextDelay(
            const std::chrono::seconds& delay)
        {
            if (delay <= std::chrono::seconds::zero())
            {
                return std::chrono::steady_clock::duration::zero();
            }
            return std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(delay);
        }

        std::optional<PluginPackageDesc> ReadPluginManifestDesc(
            const std::filesystem::path& manifest_path)
        {
            const std::ifstream ifs(manifest_path);
            if (!ifs.is_open())
            {
                return std::nullopt;
            }

            std::stringstream ss;
            ss << ifs.rdbuf();
            auto parsed = Das::Utils::ParseYyjsonFromString(
                ss.str(),
                yyjson::ReadFlag::AllowComments
                    | yyjson::ReadFlag::AllowTrailingCommas);
            if (!parsed)
            {
                return std::nullopt;
            }

            const auto& const_value = *parsed;
            auto        obj = const_value.as_object();
            if (!obj)
            {
                return std::nullopt;
            }

            PluginPackageDesc desc;
            ParsePluginPackageDescFromJson(*obj, desc);
            return desc;
        }
    } // namespace

    // ----------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------

    static std::string GuidToString(const DasGuid& guid)
    {
        return DAS_FMT_NS::format("{}", guid);
    }

    static std::chrono::steady_clock::duration ComputeNextDelay(
        const std::vector<TaskInstanceRecord>& task_instances,
        int64_t                                now_unix)
    {
        bool    has_future_due = false;
        int64_t earliest_due = 0;

        for (const auto& inst : task_instances)
        {
            if (!inst.enabled
                || inst.availability != TaskAvailability::Available
                || !inst.task_instance)
            {
                continue;
            }

            if (!inst.next_execution_time)
            {
                return std::chrono::steady_clock::duration::zero();
            }

            const int64_t due = *inst.next_execution_time;

            if (due <= now_unix)
            {
                return std::chrono::steady_clock::duration::zero();
            }

            if (!has_future_due || due < earliest_due)
            {
                earliest_due = due;
                has_future_due = true;
            }
        }

        if (!has_future_due)
        {
            return std::chrono::seconds(1);
        }

        return ClampNextDelay(std::chrono::seconds(earliest_due - now_unix));
    }

    template <typename Json>
    static yyjson::value CloneJsonValue(const Json& value)
    {
        return Das::Utils::CloneYyjsonValue(value);
    }

    static DasPtr<Das::ExportInterface::IDasJson> WrapJsonValue(
        yyjson::value value)
    {
        return Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
            std::move(value));
    }

    static yyjson::value ReadJsonInterface(Das::ExportInterface::IDasJson* json)
    {
        if (!json)
        {
            return Das::Utils::MakeYyjsonObject();
        }

        DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put())) || !text)
        {
            return Das::Utils::MakeYyjsonObject();
        }

        const char* raw = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&raw)) || raw == nullptr)
        {
            return Das::Utils::MakeYyjsonObject();
        }

        auto parsed = Das::Utils::ParseYyjsonFromString(raw);
        if (!parsed)
        {
            return Das::Utils::MakeYyjsonObject();
        }
        return std::move(*parsed);
    }

    static yyjson::value MakeAuthoringResponseError(
        std::string_view     kind,
        std::string_view     message,
        const yyjson::value* diagnostics = nullptr)
    {
        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("ok")] = false;
        (*obj)[std::string_view("errorKind")] = std::string(kind);
        (*obj)[std::string_view("message")] = std::string(message);
        if (diagnostics != nullptr && diagnostics->as_array().has_value())
        {
            (*obj)[std::string_view("diagnostics")] =
                CloneJsonValue(*diagnostics);
        }
        return result;
    }

    static yyjson::value MakeRepositoryResponseError(
        std::string_view     kind,
        std::string_view     message,
        const yyjson::value* diagnostics = nullptr)
    {
        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("ok")] = false;
        (*obj)[std::string_view("errorKind")] = std::string(kind);
        (*obj)[std::string_view("message")] = std::string(message);
        if (diagnostics != nullptr && diagnostics->as_array().has_value())
        {
            (*obj)[std::string_view("diagnostics")] =
                CloneJsonValue(*diagnostics);
        }
        return result;
    }

    static std::string RepositoryErrorKindFromResult(DasResult result)
    {
        switch (result)
        {
        case DAS_E_INVALID_JSON:
            return "invalidJson";
        case DAS_E_TYPE_ERROR:
            return "invalidProperties";
        case DAS_E_NOT_FOUND:
            return "notFound";
        case DAS_E_TASK_WORKING:
            return "taskWorking";
        case DAS_E_OBJECT_NOT_INIT:
            return "notInitialized";
        default:
            return "repositoryError";
        }
    }

    static bool ParseCreateRepositoryEntryRequest(
        const yyjson::value&                              request,
        Repository::Dto::CreateRepositoryEntryRequestDto& out_request)
    {
        auto obj = request.as_object();
        if (!obj)
        {
            return false;
        }

        auto plugin_guid = (*obj)[std::string_view("pluginGuid")].as_string();
        auto task_type_guid =
            (*obj)[std::string_view("taskTypeGuid")].as_string();
        if (!plugin_guid || !task_type_guid)
        {
            return false;
        }

        out_request.plugin_guid = std::string(*plugin_guid);
        out_request.task_type_guid = std::string(*task_type_guid);

        if (obj->contains(std::string_view("displayName"))
            && !(*obj)[std::string_view("displayName")].is_null())
        {
            auto display_name =
                (*obj)[std::string_view("displayName")].as_string();
            if (!display_name)
            {
                return false;
            }
            out_request.display_name = std::string(*display_name);
        }

        if (obj->contains(std::string_view("initialProperties")))
        {
            out_request.initial_properties =
                CloneJsonValue((*obj)[std::string_view("initialProperties")]);
        }
        else
        {
            out_request.initial_properties = yyjson::value{};
        }

        return true;
    }

    static bool ParseRenameRepositoryEntryRequest(
        const yyjson::value&                              request,
        Repository::Dto::RenameRepositoryEntryRequestDto& out_request)
    {
        auto obj = request.as_object();
        if (!obj)
        {
            return false;
        }

        auto display_name = (*obj)[std::string_view("displayName")].as_string();
        if (!display_name)
        {
            return false;
        }

        out_request.display_name = std::string(*display_name);
        return true;
    }

    static int64_t GetAuthoringRevision(const yyjson::value& task_json)
    {
        auto task_obj = task_json.as_object();
        if (!task_obj || !task_obj->contains(std::string_view("authoring")))
        {
            return 0;
        }
        auto auth_obj = (*task_obj)[std::string_view("authoring")].as_object();
        if (!auth_obj || !auth_obj->contains(std::string_view("revision")))
        {
            return 0;
        }
        auto revision = (*auth_obj)[std::string_view("revision")].as_sint();
        return revision ? *revision : 0;
    }

    // Phase 84 registry-driven actionCatalog injection (D-84-31/32/33).
    // Scheduler holds PluginManager by reference and is the natural owner of
    // the registered IDasTaskComponent component_guid snapshot. It injects the
    // catalog as a top-level `actionCatalog` JSON array of guid strings inside
    // every authoring session context. Plugins that participate in the
    // Add-Task affordance (DasGraphTask) read this array and forward it to the
    // Core via GraphAuthoringSessionSetActionCatalog; plugins that do not
    // simply ignore it. The array is sorted lexically for deterministic
    // ordering.
    static yyjson::value BuildActionCatalogFromRegistry(
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager)
    {
        auto definitions = plugin_manager.GetTaskComponentFactoryManager()
                               .EnumerateDefinitions();
        if (definitions.empty())
        {
            return yyjson::value(Das::Utils::MakeYyjsonArray());
        }

        std::vector<std::size_t> order;
        order.reserve(definitions.size());
        for (std::size_t i = 0; i < definitions.size(); ++i)
        {
            order.push_back(i);
        }
        std::sort(
            order.begin(),
            order.end(),
            [&definitions](std::size_t lhs, std::size_t rhs)
            {
                return GuidToString(definitions[lhs].component_guid)
                       < GuidToString(definitions[rhs].component_guid);
            });

        // Build a flat JSON array of guid strings. Per das-yyjson-string-safety
        // mutable arrays do not accept the (string_view, copy_string) pair
        // overload; we serialise the array via a small JSON literal reader so
        // every guid is deep-copied into the document arena.
        std::string literal = "[";
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            if (i != 0)
            {
                literal += ',';
            }
            literal += '"';
            literal += GuidToString(definitions[order[i]].component_guid);
            literal += '"';
        }
        literal += ']';

        auto parsed = Das::Utils::ParseYyjsonFromString(literal);
        if (!parsed)
        {
            return yyjson::value(Das::Utils::MakeYyjsonArray());
        }
        return std::move(*parsed);
    }

    static yyjson::value MakeAuthoringContextJson(
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
        int64_t                                         task_id,
        const yyjson::value&                            properties,
        const yyjson::value&                            task_json)
    {
        yyjson::value context(Das::Utils::MakeYyjsonObject());
        auto          obj = context.as_object();
        (*obj)[std::string_view("taskId")] = task_id;
        (*obj)[std::string_view("properties")] = CloneJsonValue(properties);
        (*obj)[std::string_view("revision")] = GetAuthoringRevision(task_json);

        auto task_obj = task_json.as_object();
        if (task_obj && task_obj->contains(std::string_view("authoring")))
        {
            (*obj)[std::string_view("authoring")] =
                CloneJsonValue((*task_obj)[std::string_view("authoring")]);
        }
        (*obj)[std::string_view("actionCatalog")] =
            BuildActionCatalogFromRegistry(plugin_manager);
        return context;
    }

    static yyjson::value MakeRepositoryAuthoringContextJson(
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
        int64_t                                         entry_id,
        const yyjson::value&                            properties,
        const yyjson::value&                            entry_json)
    {
        yyjson::value context(Das::Utils::MakeYyjsonObject());
        auto          obj = context.as_object();
        (*obj)[std::string_view("entryId")] = entry_id;
        (*obj)[std::string_view("properties")] = CloneJsonValue(properties);
        (*obj)[std::string_view("revision")] = GetAuthoringRevision(entry_json);

        auto entry_obj = entry_json.as_object();
        if (entry_obj && entry_obj->contains(std::string_view("authoring")))
        {
            (*obj)[std::string_view("authoring")] =
                CloneJsonValue((*entry_obj)[std::string_view("authoring")]);
        }
        (*obj)[std::string_view("actionCatalog")] =
            BuildActionCatalogFromRegistry(plugin_manager);
        return context;
    }

    static std::optional<int64_t> GetBaseRevision(const yyjson::value& change)
    {
        auto obj = change.as_object();
        if (!obj || !obj->contains(std::string_view("baseRevision")))
        {
            return std::nullopt;
        }
        auto revision = (*obj)[std::string_view("baseRevision")].as_sint();
        if (!revision)
        {
            return std::nullopt;
        }
        return *revision;
    }

    static std::string GetStringField(
        const yyjson::value& value,
        std::string_view     key,
        std::string_view     fallback)
    {
        auto obj = value.as_object();
        if (!obj || !obj->contains(key))
        {
            return std::string(fallback);
        }
        auto str = (*obj)[key].as_string();
        if (!str)
        {
            return std::string(fallback);
        }
        return std::string(*str);
    }

    static bool IsExplicitFalse(const yyjson::value& value)
    {
        auto bool_value = value.as_bool();
        return bool_value && !*bool_value;
    }

    static bool GetBoolField(
        const yyjson::value& value,
        std::string_view     key,
        bool                 fallback)
    {
        auto obj = value.as_object();
        if (!obj || !obj->contains(key))
        {
            return fallback;
        }

        auto bool_value = (*obj)[key].as_bool();
        return bool_value ? *bool_value : fallback;
    }

    // ===================================================================
    // Phase 84 shared Authoring result validation/persistence helpers
    // -------------------------------------------------------------------
    // D-84-08/09/10/11: Scheduler is a transparent validator that preserves
    // the Provider result envelope. taskId and repository entryId adapters
    // share the same parsing, diagnostic validation, returned-revision
    // assertion, and prepare-then-write order; they differ only in the
    // target lookup and store adapters.
    // ===================================================================

    struct AuthoringProviderGet
    {
        yyjson::value document;
        yyjson::value diagnostics;
        bool          has_document = false;
        std::string   error_kind;
        std::string   error_message;
    };

    static AuthoringProviderGet ParseAuthoringProviderGet(
        const yyjson::value& provider_result)
    {
        AuthoringProviderGet outcome;
        auto validation = ValidateAuthoringGetResult(provider_result);
        if (!validation.valid)
        {
            outcome.error_kind = validation.error_kind;
            outcome.error_message = validation.message;
            return outcome;
        }

        auto obj = provider_result.as_object();
        if (obj && obj->contains(std::string_view("document")))
        {
            outcome.document =
                CloneJsonValue((*obj)[std::string_view("document")]);
            outcome.has_document = true;
            auto document_validation =
                ValidateAuthoringDocument(outcome.document);
            if (!document_validation.valid)
            {
                outcome.error_kind = document_validation.error_kind;
                outcome.error_message = document_validation.message;
                return outcome;
            }
        }
        outcome.diagnostics =
            CloneJsonValue((*obj)[std::string_view("diagnostics")]);
        return outcome;
    }

    struct AuthoringProviderApply
    {
        yyjson::value accepted_properties;
        yyjson::value document;
        yyjson::value diagnostics;
        int64_t       returned_revision = 0;
        std::string   error_kind;
        std::string   error_message;
    };

    // 短路 kind：Apply 响应不带 document，前端据有无 document 分流
    // （UiEventApplied/ChangeApplied）。目前仅 uiEvent；其余为文档编辑 kind，
    // 缺 document 视为 provider 未执行变更（missingField，不落盘）。
    static bool IsShortCircuitAuthoringKind(std::string_view kind)
    {
        return kind == "uiEvent";
    }

    static AuthoringProviderApply ParseAuthoringProviderApply(
        const yyjson::value& provider_result,
        int64_t              base_revision,
        std::string_view     change_kind)
    {
        AuthoringProviderApply outcome;
        // Capture diagnostics before validation so a provider-rejected apply
        // (error diagnostics) can surface its real reason even if the envelope
        // later fails a stricter check.
        auto pre_obj = provider_result.as_object();
        if (pre_obj && pre_obj->contains(std::string_view("diagnostics")))
        {
            outcome.diagnostics =
                CloneJsonValue((*pre_obj)[std::string_view("diagnostics")]);
        }
        auto validation =
            ValidateAuthoringApplyResult(provider_result, base_revision);
        if (!validation.valid)
        {
            outcome.error_kind = validation.error_kind;
            outcome.error_message = validation.message;
            return outcome;
        }

        auto obj = provider_result.as_object();
        outcome.accepted_properties =
            CloneJsonValue((*obj)[std::string_view("acceptedProperties")]);

        // document 对短路 kind（uiEvent 等）可选：其响应不带 document（前端
        // 据有无 document 分流）。文档编辑 kind 必须回传 document——有则解析
        // revision + 校验 document；无则按 base+1 推进 revision（与前端一致，
        // ui.values 已变更）。
        const bool has_document =
            obj->contains(std::string_view("document"))
            && !(*obj)[std::string_view("document")].is_null();
        if (has_document)
        {
            outcome.document =
                CloneJsonValue((*obj)[std::string_view("document")]);
            auto       doc_obj = outcome.document.as_object();
            const auto revision_view = (*doc_obj)[std::string_view("revision")];
            outcome.returned_revision =
                revision_view.is_sint()
                    ? revision_view.as_sint().value()
                    : static_cast<int64_t>(revision_view.as_uint().value());

            auto document_validation =
                ValidateAuthoringDocument(outcome.document);
            if (!document_validation.valid)
            {
                outcome.error_kind = document_validation.error_kind;
                outcome.error_message = document_validation.message;
                return outcome;
            }
        }
        else if (!IsShortCircuitAuthoringKind(change_kind))
        {
            outcome.error_kind = "missingField";
            outcome.error_message =
                "AuthoringApplyResult.document required for kind "
                + std::string(change_kind);
            return outcome;
        }
        else
        {
            outcome.returned_revision = base_revision + 1;
        }
        return outcome;
    }

    struct AuthoringProviderCompile
    {
        yyjson::value compile;
        yyjson::value diagnostics;
        std::string   error_kind;
        std::string   error_message;
    };

    static AuthoringProviderCompile ParseAuthoringProviderCompile(
        const yyjson::value& provider_result)
    {
        AuthoringProviderCompile outcome;
        auto validation = ValidateAuthoringCompileResult(provider_result);
        if (!validation.valid)
        {
            outcome.error_kind = validation.error_kind;
            outcome.error_message = validation.message;
            return outcome;
        }

        auto obj = provider_result.as_object();
        outcome.compile = CloneJsonValue((*obj)[std::string_view("compile")]);
        outcome.diagnostics =
            CloneJsonValue((*obj)[std::string_view("diagnostics")]);
        return outcome;
    }

    // Builds the persisted authoring metadata object. Per D-84-02/D-84-04 the
    // metadata records only revision and an optional lastChangeKind; migration
    // and migrationState are removed from the authority. sourceFingerprint is
    // preserved best-effort because the RepositoryInvokeCompiler still consumes
    // it as an entry-identity guard, and RepositoryAuthoringMetadataDto still
    // requires the key when reading older entries.
    static yyjson::value MakePersistedAuthoringMetadata(
        int64_t              revision,
        std::string_view     last_change_kind,
        const yyjson::value& existing_authoring)
    {
        yyjson::value authoring(Das::Utils::MakeYyjsonObject());
        auto          obj = authoring.as_object();
        (*obj)[std::string_view("revision")] = revision;
        if (!last_change_kind.empty())
        {
            (*obj)[std::string_view("lastChangeKind")] =
                std::string(last_change_kind);
        }
        else if (
            auto prev = existing_authoring.as_object();
            prev && prev->contains(std::string_view("lastChangeKind")))
        {
            (*obj)[std::string_view("lastChangeKind")] =
                CloneJsonValue((*prev)[std::string_view("lastChangeKind")]);
        }
        if (auto prev = existing_authoring.as_object();
            prev && prev->contains(std::string_view("sourceFingerprint")))
        {
            (*obj)[std::string_view("sourceFingerprint")] =
                CloneJsonValue((*prev)[std::string_view("sourceFingerprint")]);
        }
        else
        {
            (*obj)[std::string_view("sourceFingerprint")] = std::string("");
        }
        return authoring;
    }

    static yyjson::value ExtractProviderExecutionInput(
        const yyjson::value& compile)
    {
        auto obj = compile.as_object();
        if (obj && obj->contains(std::string_view("executionInput")))
        {
            return CloneJsonValue((*obj)[std::string_view("executionInput")]);
        }
        return yyjson::value{};
    }

    static yyjson::value MakeExecutionCompileRequest()
    {
        yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto          obj = request.as_object();
        (*obj)[std::string_view("purpose")] = "execution";
        return request;
    }

    static DasResult CreateAuthoringSessionWithContext(
        Das::Core::ForeignInterfaceHost::PluginManager&         plugin_manager,
        const TaskAuthoringCapability&                          capability,
        yyjson::value                                           context,
        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession>& session)
    {
        DasPtr<Das::PluginInterface::IDasTaskAuthoringSessionFactory> factory;
        auto factory_features = plugin_manager.GetFeaturesByType(
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);
        for (auto* feature_info : factory_features)
        {
            if (!feature_info
                || feature_info->plugin_guid != capability.plugin_guid
                || !feature_info->interface_ptr)
            {
                continue;
            }

            DasPtr<Das::PluginInterface::IDasTaskAuthoringSessionFactory>
                 candidate;
            auto query_result = feature_info->interface_ptr->QueryInterface(
                DasIidOf<
                    Das::PluginInterface::IDasTaskAuthoringSessionFactory>(),
                reinterpret_cast<void**>(candidate.Put()));
            if (DAS::IsFailed(query_result) || !candidate)
            {
                continue;
            }

            DasGuid candidate_guid{};
            auto    guid_result = candidate->GetGuid(&candidate_guid);
            if (DAS::IsFailed(guid_result))
            {
                continue;
            }

            if (candidate_guid == capability.authoring_factory_guid)
            {
                factory = std::move(candidate);
                break;
            }
        }

        if (!factory)
        {
            return DAS_E_NOT_FOUND;
        }

        auto context_json = WrapJsonValue(std::move(context));
        return factory->CreateSession(
            capability.task_guid,
            context_json.Get(),
            session.Put());
    }

    // Cache core, shared by the member GetOrCreateAuthoringSession (Task/Repo
    // Get + Compile callers, which resolve the session before any pipeline)
    // and by the Apply pipeline, which must resolve the session AFTER its ③④
    // envelope/revision pre-checks (D-85-02c: an envelope failure must not
    // create a session). `mutex` guards only find/emplace; session creation
    // happens outside the lock (v1.3 short-critical-section rule). Returns
    // null on create failure without inserting an empty entry.
    static std::shared_ptr<AuthoringSessionEntry> ResolveCachedAuthoringSession(
        std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>&
                                                        cache,
        std::mutex&                                     mutex,
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
        const TaskAuthoringCapability&                  capability,
        int64_t                                         id,
        yyjson::value                                   context,
        int64_t                                         current_revision,
        bool                                            enforce_fresh)
    {
        {
            const std::scoped_lock lock(mutex);
            auto                   it = cache.find(id);
            if (it != cache.end())
            {
                // Read-side freshness: a Get following an Apply must rebuild
                // because the provider's document view is built from the
                // context captured at session creation. Apply skips this
                // (enforce_fresh=false) so consecutive Applies reuse one
                // session.
                if (enforce_fresh
                    && it->second->creation_revision != current_revision)
                {
                    cache.erase(it);
                }
                else
                {
                    return it->second;
                }
            }
        }

        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
        auto session_result = CreateAuthoringSessionWithContext(
            plugin_manager,
            capability,
            std::move(context),
            session);
        if (DAS::IsFailed(session_result) || !session)
        {
            return nullptr;
        }

        auto entry = std::make_shared<AuthoringSessionEntry>();
        entry->session = std::move(session);
        entry->creation_revision = current_revision;

        // emplace (not operator[]) so a concurrent inserter wins and ours is
        // dropped: avoids overwriting a live entry.
        const std::scoped_lock lock(mutex);
        auto                   emplace_result = cache.emplace(id, entry);
        return emplace_result.first->second;
    }

    // Structured result of the shared Apply pipeline. The caller owns response
    // construction (taskId vs entryId) and path-specific error message wording
    // for revisionConflict / sessionCreateFailed / providerFailed kinds.
    struct AuthoringApplyPipelineResult
    {
        bool                   ok = false;
        std::string            error_kind;
        std::string            error_message;
        int64_t                current_revision = 0;
        int64_t                next_revision = 0;
        AuthoringProviderApply parsed;
        yyjson::value          authoring;
    };

    // Locked core of the Apply pipeline. The caller has already done the
    // envelope validation, the revision check on a fresh settings read, and the
    // cache resolve, and holds `entry->lock` (unique_lock) so this Provider
    // ApplyChange (M) + parse + rejection check + persisted-metadata build runs
    // atomically with the caller's persist (W). The whole M-W section lives
    // outside mutex_ and inside entry->lock (D-87-07 mutex-not-abused).
    // Returns true on success (result.parsed/next_revision/authoring filled);
    // false on provider/parse/rejection error (result.error_kind/message set,
    // result.parsed stays populated so an applyRejected response can carry the
    // diagnostics array).
    static bool RunAuthoringApplyPipelineLocked(
        const std::shared_ptr<AuthoringSessionEntry>& entry,
        const yyjson::value&                          change,
        const yyjson::value&                          source_json,
        int64_t                                       current_revision,
        AuthoringApplyPipelineResult&                 result)
    {
        auto& session = entry->session;

        auto change_json = WrapJsonValue(CloneJsonValue(change));
        DasPtr<Das::ExportInterface::IDasJson> apply_result_json;
        auto                                   apply_result =
            session->ApplyChange(change_json.Get(), apply_result_json.Put());
        if (DAS::IsFailed(apply_result) || !apply_result_json)
        {
            result.error_kind = "providerFailed";
            result.error_message = "Authoring provider rejected change";
            return false;
        }

        auto       provider_result = ReadJsonInterface(apply_result_json.Get());
        const auto change_kind = GetStringField(change, "kind", "");
        auto       parsed = ParseAuthoringProviderApply(
            provider_result,
            current_revision,
            change_kind);
        if (!parsed.error_kind.empty())
        {
            result.error_kind = parsed.error_kind;
            result.error_message = parsed.error_message;
            return false;
        }
        result.parsed = std::move(parsed);

        // Provider-rejected apply: the envelope is valid but the provider
        // declined the change (error diagnostics present). Do not advance the
        // store (no persist, no revision bump); surface the real rejection
        // reason. result.parsed stays populated so the response carries the
        // diagnostics array.
        if (HasErrorDiagnostic(result.parsed.diagnostics))
        {
            result.error_kind = "applyRejected";
            result.error_message =
                FirstErrorDiagnosticMessage(result.parsed.diagnostics);
            if (result.error_message.empty())
            {
                result.error_message = "Authoring change rejected by provider";
            }
            return false;
        }

        result.next_revision = result.parsed.returned_revision;

        yyjson::value previous_authoring;
        if (auto src_obj = source_json.as_object();
            src_obj && src_obj->contains(std::string_view("authoring")))
        {
            previous_authoring =
                CloneJsonValue((*src_obj)[std::string_view("authoring")]);
        }
        result.authoring = MakePersistedAuthoringMetadata(
            result.next_revision,
            change_kind,
            previous_authoring);

        result.ok = true;
        return true;
    }

    // Structured result of the shared Get pipeline. The caller owns response
    // construction (taskId vs entryId/taskTypeGuid/revision) and path-specific
    // error message wording for sessionCreateFailed / providerFailed kinds.
    struct AuthoringGetPipelineResult
    {
        bool                 ok = false;
        std::string          error_kind;
        std::string          error_message;
        AuthoringProviderGet parsed;
    };

    // Shared Get pipeline (Provider GetDocument -> parse). The caller owns
    // ①② locks/IO/context-build and resolves the cached session via
    // GetOrCreateAuthoringSession; this pipeline owns ⑥⑦(GetDocument + parse).
    static AuthoringGetPipelineResult RunAuthoringGetPipeline(
        std::shared_ptr<AuthoringSessionEntry> entry,
        const yyjson::value&                   request)
    {
        AuthoringGetPipelineResult result;

        if (!entry || !entry->session)
        {
            result.error_kind = "sessionCreateFailed";
            result.error_message = "Failed to create authoring session";
            return result;
        }

        auto& session = entry->session;

        auto request_json = WrapJsonValue(CloneJsonValue(request));
        DasPtr<Das::ExportInterface::IDasJson> document_json;
        auto                                   document_result =
            session->GetDocument(request_json.Get(), document_json.Put());
        if (DAS::IsFailed(document_result) || !document_json)
        {
            result.error_kind = "providerFailed";
            result.error_message =
                "Authoring provider failed to create document";
            return result;
        }

        auto provider_result = ReadJsonInterface(document_json.Get());
        auto parsed = ParseAuthoringProviderGet(provider_result);
        if (!parsed.error_kind.empty())
        {
            result.error_kind = parsed.error_kind;
            result.error_message = parsed.error_message;
            return result;
        }
        result.parsed = std::move(parsed);

        result.ok = true;
        return result;
    }

    // Structured result of the shared Compile pipeline. The caller owns
    // response construction (taskId vs entryId) and path-specific error message
    // wording for sessionCreateFailed / providerFailed kinds.
    struct AuthoringCompilePipelineResult
    {
        bool                     ok = false;
        std::string              error_kind;
        std::string              error_message;
        AuthoringProviderCompile parsed;
    };

    // Shared Compile pipeline (session create -> Provider Compile -> parse).
    // Caller owns ①② locks/IO/context-build; pipeline owns ⑤⑥⑦. Session is
    // created inside the pipeline, mirroring the Apply/Get pipeline ordering.
    static AuthoringCompilePipelineResult RunAuthoringCompilePipeline(
        std::shared_ptr<AuthoringSessionEntry> entry,
        const yyjson::value&                   request)
    {
        AuthoringCompilePipelineResult result;

        if (!entry || !entry->session)
        {
            result.error_kind = "sessionCreateFailed";
            result.error_message = "Failed to create authoring session";
            return result;
        }
        auto& session = entry->session;

        auto request_json = WrapJsonValue(CloneJsonValue(request));
        DasPtr<Das::ExportInterface::IDasJson> compile_json;
        auto                                   compile_result =
            session->Compile(request_json.Get(), compile_json.Put());
        if (DAS::IsFailed(compile_result) || !compile_json)
        {
            result.error_kind = "providerFailed";
            result.error_message =
                "Authoring provider failed to compile document";
            return result;
        }

        auto provider_result = ReadJsonInterface(compile_json.Get());
        auto parsed = ParseAuthoringProviderCompile(provider_result);
        if (!parsed.error_kind.empty())
        {
            result.error_kind = parsed.error_kind;
            result.error_message = parsed.error_message;
            return result;
        }
        result.parsed = std::move(parsed);

        result.ok = true;
        return result;
    }

    struct RepositoryProviderCompileResult
    {
        yyjson::value compile;
        std::string   error_kind;
        std::string   message;

        bool Succeeded() const { return error_kind.empty(); }
    };

    static RepositoryProviderCompileResult CompileRepositoryEntryProvider(
        std::shared_ptr<AuthoringSessionEntry> entry,
        const yyjson::value&                   request)
    {
        if (!entry || !entry->session)
        {
            return {
                {},
                "sessionCreateFailed",
                "Failed to create repository authoring session"};
        }
        auto& session = entry->session;

        auto request_json = WrapJsonValue(CloneJsonValue(request));
        DasPtr<Das::ExportInterface::IDasJson> compile_json;
        auto                                   compile_result =
            session->Compile(request_json.Get(), compile_json.Put());
        if (DAS::IsFailed(compile_result) || !compile_json)
        {
            return {
                {},
                "providerFailed",
                "Repository authoring provider failed to compile document"};
        }

        return {ReadJsonInterface(compile_json.Get()), {}, {}};
    }

    static TaskInstanceRecord* FindNextRunnableTask(
        std::vector<TaskInstanceRecord>& task_instances,
        int64_t                          now_unix)
    {
        TaskInstanceRecord* selected = nullptr;
        int64_t             selected_due = 0;
        bool                selected_has_due = false;

        for (auto& inst : task_instances)
        {
            if (!inst.enabled
                || inst.availability != TaskAvailability::Available
                || !inst.task_instance)
            {
                continue;
            }

            if (!inst.next_execution_time)
            {
                return &inst;
            }

            const int64_t due = *inst.next_execution_time;

            if (due > now_unix)
            {
                continue;
            }

            if (!selected || !selected_has_due || due < selected_due)
            {
                selected = &inst;
                selected_due = due;
                selected_has_due = true;
            }
        }

        return selected;
    }

    TaskTypeRecord* SchedulerService::FindTaskType(const DasGuid& task_guid)
    {
        for (auto& ttr : task_types_)
        {
            if (ttr->task_guid == task_guid)
            {
                return ttr.get();
            }
        }
        return nullptr;
    }

    TaskInstanceRecord* SchedulerService::FindTaskInstance(int64_t task_id)
    {
        for (auto& inst : task_instances_)
        {
            if (inst.id == task_id)
            {
                return &inst;
            }
        }
        return nullptr;
    }

    const TaskInstanceRecord* SchedulerService::FindTaskInstance(
        int64_t task_id) const
    {
        for (const auto& inst : task_instances_)
        {
            if (inst.id == task_id)
            {
                return &inst;
            }
        }
        return nullptr;
    }

    Repository::Dto::RepositoryAvailabilityDto
    SchedulerService::DeriveRepositoryAvailabilityLocked(
        const Repository::Dto::RepositoryEntryDto& entry)
    {
        Repository::Dto::RepositoryAvailabilityDto availability;

        DasGuid plugin_guid{};
        DasGuid task_guid{};
        try
        {
            plugin_guid =
                Das::Core::ForeignInterfaceHost::MakeDasGuid(entry.plugin_guid);
            task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                entry.task_type_guid);
        }
        catch (const std::exception&)
        {
            availability.state = "unavailable";
            availability.reason = "pluginUnavailable";
            availability.message =
                "Repository entry contains an invalid plugin or task GUID";
            return availability;
        }

        if (auto unavailable_plugin =
                repository_plugin_availability_.find(plugin_guid);
            unavailable_plugin != repository_plugin_availability_.end())
        {
            availability.state = "unavailable";
            availability.reason = unavailable_plugin->second.reason;
            availability.message = unavailable_plugin->second.message;
            return availability;
        }

        if (plugin_manager_.FindPluginPackageByGuid(plugin_guid) == nullptr)
        {
            availability.state = "unavailable";
            availability.reason = "pluginUnavailable";
            availability.message =
                "Plugin package is not loaded for the initialized profile";
            return availability;
        }

        auto* task_record = capability_registry_.FindTask(task_guid);
        if (!task_record || task_record->plugin_guid != plugin_guid)
        {
            availability.state = "unavailable";
            availability.reason = "taskTypeUnavailable";
            availability.message =
                "Task type is not declared by the initialized plugin package";
            return availability;
        }

        if (capability_registry_.FindAuthoring(task_guid) == nullptr)
        {
            availability.state = "unavailable";
            availability.reason = "capabilityMissing";
            availability.message =
                "Task type does not declare an authoring capability";
            return availability;
        }

        availability.state = "available";
        return availability;
    }

    std::optional<AuthoringGuardError>
    SchedulerService::CheckSchedulerIdleLocked() const
    {
        if (state_.load() == SchedulerState::Running
            || state_.load() == SchedulerState::Stopping)
        {
            return AuthoringGuardError{
                "taskWorking",
                "Scheduler is running or stopping"};
        }
        return std::nullopt;
    }

    std::optional<AuthoringGuardError>
    SchedulerService::ResolveAuthoringCapabilityLocked(
        const DasGuid&           task_guid,
        TaskAuthoringCapability& out_capability)
    {
        auto* authoring = capability_registry_.FindAuthoring(task_guid);
        if (!authoring)
        {
            return AuthoringGuardError{
                "capabilityMissing",
                "Task does not declare authoring capability"};
        }
        out_capability = *authoring;
        return std::nullopt;
    }

    // ----------------------------------------------------------------
    // Construction
    // ----------------------------------------------------------------

    SchedulerService::SchedulerService(
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
        Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context)
        : plugin_manager_(plugin_manager), ipc_context_{std::move(ipc_context)}
    {
        config_persist_thread_ =
            std::thread(&SchedulerService::ConfigPersistThreadLoop, this);
        // D-87-08: 注册为 PluginManager 的 lifecycle listener，plugin
        // load/unload 后全清 authoring cache。SchedulerService 持有
        // PluginManager& 引用，结构上 PluginManager 存活长于
        // SchedulerService，裸指针注册安全。
        plugin_manager_.SetPluginLifecycleListener(this);
    }

    SchedulerService::~SchedulerService()
    {
        // 防御：清注册。PluginManager 结构上存活长于 SchedulerService（持有
        // PluginManager& 引用），此调用成本为零；若实际析构顺序变化也避免
        // PluginManager 持悬空 listener 指针。
        plugin_manager_.SetPluginLifecycleListener(nullptr);
        ShutdownConfigPersistQueue();
        if (disable_thread_.joinable())
        {
            disable_thread_.join();
        }
    }

    void SchedulerService::OnPluginLifecycleEvent(
        Das::Core::ForeignInterfaceHost::PluginLifecyclePhase /*phase*/,
        const DasGuid& /*plugin_guid*/)
    {
        // Initialize() has already cleared both caches while holding mutex_.
        // PluginManager may notify synchronously from the initiating thread or
        // concurrently from a Host callback. In either case, acquiring mutex_
        // here is redundant and may deadlock with the active unload path.
        if (suppress_plugin_lifecycle_invalidation_.load())
        {
            return;
        }

        // Any plugin change outside the SS-initiated window invalidates every
        // authoring session so the next access rebuilds against fresh state.
        InvalidateAuthoringSessions();
    }

    DasResult SchedulerService::CreateTaskInstance(
        const TaskTypeRecord&            task_type,
        Das::PluginInterface::IDasTask** pp_out_task)
    {
        if (!pp_out_task)
        {
            return DAS_E_INVALID_POINTER;
        }

        DasOutPtr<Das::PluginInterface::IDasTask> result(pp_out_task);

        auto create_result = plugin_manager_.CreateFeatureInterface(
            task_type.plugin_guid,
            task_type.feature_index,
            DasIidOf<Das::PluginInterface::IDasTask>(),
            reinterpret_cast<void**>(result.Put()));
        if (IsOk(create_result) && result)
        {
            result.Keep();
            return create_result;
        }

        if (create_result != DAS_E_NOT_FOUND || !task_type.prototype_task)
        {
            return create_result;
        }

        result.Set(task_type.prototype_task.Get());
        result.Keep();
        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // Initialize
    // ----------------------------------------------------------------

    DasResult SchedulerService::Initialize(
        const std::filesystem::path& plugin_dir,
        const std::vector<DasGuid>&  disabled_guids)
    {
        // Phase 1: Short lock for state validation and cleanup
        {
            const std::scoped_lock lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                DAS_CORE_LOG_ERROR(
                    "SchedulerService::Initialize: reject, scheduler is "
                    "Running or Stopping");
                return DAS_E_TASK_WORKING;
            }

            if (initialized_)
            {
                // Re-initialize: clear existing runtime state
                task_types_.clear();
                task_instances_.clear();
                task_repository_store_.reset();
                capability_registry_.Clear();
                repository_plugin_availability_.clear();
                InvalidateAuthoringSessionsLocked();

                const Das::Utils::ScopeGuardVoid lifecycle_invalidation_guard{
                    [this]()
                    { suppress_plugin_lifecycle_invalidation_.store(true); },
                    [this]()
                    { suppress_plugin_lifecycle_invalidation_.store(false); }};
                for (auto& loaded_plugin_path :
                     std::views::reverse(loaded_plugin_paths_))
                {
                    plugin_manager_.UnloadPlugin(loaded_plugin_path);
                }
                loaded_plugin_paths_.clear();
                initialized_ = false;
            }
        } // lock released

        // Phase 2: Lock-free I/O — directory traversal, manifest reading,
        // plugin loading, and SettingsManager reads all execute outside
        // mutex_.
        std::error_code ec;
        auto            dir_iter = std::filesystem::directory_iterator(
            plugin_dir,
            std::filesystem::directory_options::skip_permission_denied,
            ec);
        if (ec)
        {
            DAS_CORE_LOG_WARN(
                "SchedulerService::Initialize: failed to iterate "
                "plugin directory {}: {}",
                DAS::Utils::U8AsString(plugin_dir.u8string()),
                ec.message());
            return DAS_E_FAIL;
        }

        // Temp vectors to accumulate loaded data outside the lock
        std::vector<std::filesystem::path>           temp_paths;
        std::vector<std::unique_ptr<TaskTypeRecord>> temp_types;
        std::unordered_map<DasGuid, RepositoryPluginAvailabilityState>
            temp_repository_plugin_availability;

        // Load allowed plugin packages
        for (const auto& entry : dir_iter)
        {
            std::filesystem::path manifest_path;
            if (entry.is_directory())
            {
                auto dirname = std::string{
                    DAS::Utils::U8AsString(entry.path().filename().u8string())};
                auto marker = entry.path() / (dirname + ".willBeDelete");
                if (std::filesystem::exists(marker))
                {
                    auto banned_manifest_path = FindManifest(entry.path());
                    if (!banned_manifest_path.empty())
                    {
                        try
                        {
                            if (auto desc = ReadPluginManifestDesc(
                                    banned_manifest_path))
                            {
                                temp_repository_plugin_availability[desc->guid] =
                                    {"pluginUnavailable",
                                     "Plugin package is marked for deletion"};
                            }
                        }
                        catch (const std::exception& e)
                        {
                            DAS_CORE_LOG_WARN(
                                "SchedulerService::Initialize: failed to "
                                "parse banned plugin manifest: {}",
                                e.what());
                        }
                    }
                    continue;
                }

                manifest_path = FindManifest(entry.path());
            }
            else if (entry.path().extension() == ".json")
            {
                manifest_path = entry.path();
            }

            if (manifest_path.empty())
            {
                continue;
            }

            try
            {
                auto desc_opt = ReadPluginManifestDesc(manifest_path);
                if (!desc_opt)
                {
                    DAS_CORE_LOG_WARN(
                        "SchedulerService::Initialize: failed to "
                        "parse manifest: {}",
                        DAS::Utils::U8AsString(manifest_path.u8string()));
                    continue;
                }

                PluginPackageDesc desc = std::move(*desc_opt);

                if (std::find(
                        disabled_guids.begin(),
                        disabled_guids.end(),
                        desc.guid)
                    != disabled_guids.end())
                {
                    temp_repository_plugin_availability[desc.guid] = {
                        "pluginUnavailable",
                        "Plugin package is disabled for scheduler initialize"};
                    DAS_CORE_LOG_INFO(
                        "SchedulerService::Initialize: skipping "
                        "disabled plugin: {}",
                        desc.name);
                    continue;
                }

                auto load_result = plugin_manager_.LoadPlugin(manifest_path);
                if (DAS::IsFailed(load_result))
                {
                    temp_repository_plugin_availability[desc.guid] = {
                        "pluginLoadFailed",
                        "Plugin package failed to load during scheduler "
                        "initialize"};
                    DAS_CORE_LOG_WARN(
                        "SchedulerService::Initialize: failed to "
                        "load plugin {}, result={}",
                        desc.name,
                        load_result);
                    continue;
                }

                temp_paths.push_back(manifest_path);
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::Initialize: failed to parse "
                    "manifest: {}",
                    e.what());
            }
        }

        // Collect IDasTask features and build available task types
        auto task_features = plugin_manager_.GetFeaturesByType(
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK);
        for (auto* feature_info : task_features)
        {
            DasPtr<Das::PluginInterface::IDasTask> task;
            auto qi_result = feature_info->interface_ptr->QueryInterface(
                DasIidOf<Das::PluginInterface::IDasTask>(),
                reinterpret_cast<void**>(task.Put()));
            if (!IsOk(qi_result) || !task)
            {
                continue;
            }

            auto type_record = std::make_unique<TaskTypeRecord>();
            type_record->prototype_task = task;
            type_record->plugin_guid = feature_info->plugin_guid;
            type_record->feature_index = feature_info->feature_index;

            // Get task type GUID from IDasTypeInfo
            DasGuid task_guid{};
            auto    guid_result = task->GetGuid(&task_guid);
            if (!IsOk(guid_result))
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::Initialize: failed to get task "
                    "GUID, result={}",
                    guid_result);
                continue;
            }
            type_record->task_guid = task_guid;

            // Find manifest task descriptor metadata
            auto* pkg_desc = plugin_manager_.FindPluginPackageByGuid(
                feature_info->plugin_guid);
            if (pkg_desc)
            {
                auto td_it = pkg_desc->task_descriptors.find(task_guid);
                if (td_it != pkg_desc->task_descriptors.end())
                {
                    const auto& td = td_it->second;
                    type_record->name = td.name;
                    type_record->description = td.description;
                    type_record->game_name = td.game_name;
                    type_record->descriptors = td.descriptors;
                    capability_registry_.AddTaskDescriptor(
                        feature_info->plugin_guid,
                        task_guid,
                        feature_info->feature_index,
                        td);
                }
            }

            temp_types.push_back(std::move(type_record));
        }

        DAS_CORE_LOG_INFO(
            "SchedulerService::Initialize: discovered {} task types",
            temp_types.size());

        // Phase 3: SettingsManager reads for task instances (lock-free
        // from SchedulerService::mutex_ perspective; SettingsManager uses
        // per-key mutex internally).
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  scheduler_index = settings.GetSchedulerIndexJson("0");

        std::vector<TaskInstanceRecord> temp_instances;

        auto sched_obj = scheduler_index.as_object();
        if (sched_obj && sched_obj->contains(std::string_view("taskOrder"))
            && sched_obj->operator[](std::string_view("taskOrder")).is_array())
        {
            auto task_order_arr =
                *sched_obj->operator[](std::string_view("taskOrder"))
                     .as_array();
            for (auto&& task_id_val : task_order_arr)
            {
                if (!task_id_val.is_int())
                {
                    continue;
                }
                const int64_t tid = static_cast<int64_t>(task_id_val);

                auto instance_json = settings.GetTaskInstanceJson("0", tid);
                auto inst_obj_opt = instance_json.as_object();
                TaskInstanceRecord rec;
                rec.id = tid;

                if (!inst_obj_opt || !instance_json.is_object())
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason =
                        "task instance file is missing or corrupt";
                    temp_instances.push_back(std::move(rec));
                    continue;
                }
                const auto& inst_obj = *inst_obj_opt;

                // Parse taskGuid
                const auto task_guid_key = std::string_view("taskGuid");
                if (!inst_obj.contains(task_guid_key))
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason = "missing taskGuid";
                    temp_instances.push_back(std::move(rec));
                    continue;
                }

                auto tg_val = inst_obj[task_guid_key];
                auto tg_str = tg_val.as_string();
                if (!tg_str)
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason = "invalid taskGuid";
                    temp_instances.push_back(std::move(rec));
                    continue;
                }

                try
                {
                    rec.task_guid =
                        Das::Core::ForeignInterfaceHost::MakeDasGuid(
                            std::string(*tg_str));
                }
                catch (const std::exception& e)
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason =
                        std::string("invalid taskGuid: ") + e.what();
                    temp_instances.push_back(std::move(rec));
                    continue;
                }

                // Parse pluginGuid
                const auto plugin_guid_key = std::string_view("pluginGuid");
                if (inst_obj.contains(plugin_guid_key))
                {
                    auto pg_val = inst_obj[plugin_guid_key];
                    auto pg_str = pg_val.as_string();
                    if (pg_str)
                    {
                        try
                        {
                            rec.plugin_guid =
                                Das::Core::ForeignInterfaceHost::MakeDasGuid(
                                    std::string(*pg_str));
                        }
                        catch (const std::exception& e)
                        {
                            rec.availability = TaskAvailability::Invalid;
                            rec.unavailability_reason =
                                std::string("invalid pluginGuid: ") + e.what();
                            temp_instances.push_back(std::move(rec));
                            continue;
                        }
                    }
                    else if (!pg_val.is_null())
                    {
                        rec.availability = TaskAvailability::Invalid;
                        rec.unavailability_reason = "invalid pluginGuid";
                        temp_instances.push_back(std::move(rec));
                        continue;
                    }
                }

                // Parse nextExecutionTime
                auto net_key = std::string_view("nextExecutionTime");
                if (inst_obj.contains(net_key) && !inst_obj[net_key].is_null())
                {
                    const auto& net_val = inst_obj[net_key];
                    auto        net_int = net_val.as_sint();
                    if (net_int)
                    {
                        rec.next_execution_time = *net_int;
                    }
                }

                // Parse enabled (defaults to true when absent for backward
                // compatibility with persisted instances that predate the
                // field). Never throws: a malformed value falls back to true.
                rec.enabled = GetBoolField(instance_json, "enabled", true);

                // Parse properties
                auto props_key = std::string_view("properties");
                if (inst_obj.contains(props_key)
                    && inst_obj[props_key].is_object())
                {
                    rec.properties = CloneJsonValue(inst_obj[props_key]);
                }
                else
                {
                    rec.properties = Das::Utils::MakeYyjsonObject();
                }

                auto authoring_key = std::string_view("authoring");
                if (inst_obj.contains(authoring_key)
                    && inst_obj[authoring_key].is_object())
                {
                    rec.authoring = CloneJsonValue(inst_obj[authoring_key]);
                }

                // Link to available task type — note: FindTaskType
                // searches temp_types (not yet committed), so we
                // search it manually here
                TaskTypeRecord* type_rec = nullptr;
                for (auto& ttr : temp_types)
                {
                    if (ttr->task_guid == rec.task_guid)
                    {
                        type_rec = ttr.get();
                        break;
                    }
                }

                if (type_rec)
                {
                    rec.task_type = type_rec;
                    rec.plugin_guid = type_rec->plugin_guid;
                    auto create_result =
                        CreateTaskInstance(*type_rec, rec.task_instance.Put());
                    if (IsOk(create_result) && rec.task_instance)
                    {
                        rec.availability = TaskAvailability::Available;
                    }
                    else
                    {
                        rec.availability = TaskAvailability::Unavailable;
                        rec.unavailability_reason =
                            "failed to create task instance";
                    }
                }
                else
                {
                    rec.availability = TaskAvailability::Unavailable;
                    rec.unavailability_reason =
                        "task type not found among loaded plugins";
                }

                temp_instances.push_back(std::move(rec));
            }
        }

        // Phase 4: Short lock to commit results to runtime state
        {
            const std::scoped_lock lock(mutex_);
            task_types_ = std::move(temp_types);
            task_instances_ = std::move(temp_instances);
            task_repository_store_ = std::make_shared<TaskRepositoryStore>(
                plugin_manager_.GetSettingsManager(),
                "0");
            loaded_plugin_paths_ = std::move(temp_paths);
            repository_plugin_availability_ =
                std::move(temp_repository_plugin_availability);
            initialized_ = true;
        }

        DAS_CORE_LOG_INFO(
            "SchedulerService::Initialize: materialized {} task instances",
            task_instances_.size());

        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // Enable / Disable / Status
    // ----------------------------------------------------------------

    DasResult SchedulerService::Enable()
    {
        const std::scoped_lock lock(mutex_);

        if (state_.load() != SchedulerState::Stopped)
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Enable: invalid state, must be "
                "Stopped");
            return DAS_E_FAIL;
        }

        if (!initialized_)
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Enable: not initialized, call "
                "Initialize first");
            return DAS_E_OBJECT_NOT_INIT;
        }

        // Check for at least one available task instance
        bool has_available = false;
        for (const auto& inst : task_instances_)
        {
            if (inst.availability == TaskAvailability::Available)
            {
                has_available = true;
                break;
            }
        }
        if (!has_available)
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Enable: no available task instances");
            return DAS_E_OBJECT_NOT_INIT;
        }

        state_.store(SchedulerState::Running);

        if (!tick_timer_)
        {
            tick_timer_ = std::make_unique<boost::asio::steady_timer>(
                ipc_context_.get().GetIoContext());
        }

        const int64_t now_unix = SystemClock::to_time_t(SystemClock::now());
        StartTickTimer(ComputeNextDelay(task_instances_, now_unix));

        DAS_CORE_LOG_INFO("SchedulerService::Enable: scheduler started");
        return DAS_S_OK;
    }

    DasResult SchedulerService::Disable()
    {
        {
            const std::scoped_lock lock(mutex_);

            if (state_.load() == SchedulerState::Stopping)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::Disable: already stopping, ignored");
                return DAS_S_OK;
            }

            if (state_.load() != SchedulerState::Running)
            {
                DAS_CORE_LOG_ERROR(
                    "SchedulerService::Disable: invalid state, must be "
                    "Running");
                return DAS_E_FAIL;
            }

            state_.store(SchedulerState::Stopping);

            // Cancel the timer but keep the object alive until the scheduler is
            // torn down, otherwise Boost.Asio may still hold internal
            // references to the timer entry while draining the io_context.
            if (tick_timer_)
            {
                tick_timer_->cancel();
            }

            // Request cooperative cancellation on the stop token.
            // stop_token_ is always created via DasStopTokenImpl::Make() in
            // OnTick(), so this static_cast is safe as long as OnTick remains
            // the sole creator.
            if (stop_token_)
            {
                auto* impl = static_cast<Das::Core::Utils::DasStopTokenImpl*>(
                    stop_token_.Get());
                if (impl)
                {
                    impl->RequestStop();
                }
            }
        }

        // Spawns a dedicated thread to wait for the current task to complete.
        // The Stopping state guard above guarantees only one such thread exists
        // at any time, so defensive joinable() check is unnecessary here.
        disable_thread_ = std::thread(
            [this]
            {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return current_task_ == nullptr; });
                }

                stop_token_.Reset();
                state_.store(SchedulerState::Stopped);
                DAS_CORE_LOG_INFO(
                    "SchedulerService::Disable: scheduler stopped");
            });

        return DAS_S_OK;
    }

    SchedulerState SchedulerService::Status() const { return state_.load(); }

    std::optional<DasGuid> SchedulerService::FindTaskExecutionComponent(
        const DasGuid& task_guid) const
    {
        const std::scoped_lock lock(mutex_);
        auto*                  execution_component =
            capability_registry_.FindExecutionComponent(task_guid);
        if (execution_component == nullptr)
        {
            return std::nullopt;
        }
        return *execution_component;
    }

    // ----------------------------------------------------------------
    // Get: merged scheduler state JSON
    // ----------------------------------------------------------------

    yyjson::value SchedulerService::Get()
    {
        const std::scoped_lock lock(mutex_);

        auto result = Das::Utils::MakeYyjsonObject();
        auto result_obj = *result.as_object();

        result_obj[std::string_view("state")] = std::string_view(
            state_.load() == SchedulerState::Running    ? "running"
            : state_.load() == SchedulerState::Stopping ? "stopping"
                                                        : "stopped");

        // Available task types
        auto types_arr = Das::Utils::MakeYyjsonArray();
        auto types_arr_ref = *types_arr.as_array();
        for (const auto& ttr : task_types_)
        {
            auto type_obj = Das::Utils::MakeYyjsonObject();
            auto type_ref = *type_obj.as_object();
            type_ref[std::string_view("taskGuid")] =
                yyjson::value(GuidToString(ttr->task_guid));
            type_ref[std::string_view("pluginGuid")] =
                yyjson::value(GuidToString(ttr->plugin_guid));
            type_ref[std::string_view("name")] = yyjson::value(ttr->name);
            type_ref[std::string_view("description")] =
                yyjson::value(ttr->description);
            if (ttr->game_name)
            {
                type_ref[std::string_view("gameName")] =
                    yyjson::value(*ttr->game_name);
            }

            if (auto* authoring =
                    capability_registry_.FindAuthoring(ttr->task_guid))
            {
                type_ref[std::string_view("configurationMode")] =
                    std::string_view("authoring");
                type_ref[std::string_view("propertiesWritable")] = false;

                yyjson::value authoring_obj(Das::Utils::MakeYyjsonObject());
                auto          authoring_ref = authoring_obj.as_object();
                (*authoring_ref)[std::string_view("factoryGuid")] =
                    yyjson::value(
                        GuidToString(authoring->authoring_factory_guid));

                yyjson::value supported_kinds(Das::Utils::MakeYyjsonArray());
                auto          supported_ref = supported_kinds.as_array();
                for (const auto& kind : authoring->supported_kinds)
                {
                    supported_ref->emplace_back(yyjson::value(kind));
                }
                (*authoring_ref)[std::string_view("supportedKinds")] =
                    std::move(supported_kinds);
                type_ref[std::string_view("authoring")] =
                    std::move(authoring_obj);
            }
            else
            {
                type_ref[std::string_view("configurationMode")] =
                    std::string_view("properties");
                type_ref[std::string_view("propertiesWritable")] = true;
            }

            // Include descriptors
            auto desc_arr = Das::Utils::MakeYyjsonArray();
            auto desc_arr_ref = *desc_arr.as_array();
            for (const auto& d : ttr->descriptors)
            {
                auto desc_obj = Das::Utils::MakeYyjsonObject();
                auto desc_ref = *desc_obj.as_object();
                desc_ref[std::string_view("name")] = yyjson::value(d.name);
                desc_ref[std::string_view("type")] =
                    static_cast<int64_t>(d.type);
                desc_ref[std::string_view("required")] = d.required;
                if (d.description)
                {
                    desc_ref[std::string_view("description")] =
                        yyjson::value(*d.description);
                }
                desc_arr_ref.emplace_back(std::move(desc_obj));
            }
            type_ref[std::string_view("descriptors")] = std::move(desc_arr);
            types_arr_ref.emplace_back(std::move(type_obj));
        }
        result_obj[std::string_view("availableTaskTypes")] =
            std::move(types_arr);

        // Queued task instances
        auto tasks_arr = Das::Utils::MakeYyjsonArray();
        auto tasks_arr_ref = *tasks_arr.as_array();
        for (const auto& inst : task_instances_)
        {
            auto task_obj = Das::Utils::MakeYyjsonObject();
            auto task_ref = *task_obj.as_object();
            task_ref[std::string_view("id")] = inst.id;
            task_ref[std::string_view("taskGuid")] =
                yyjson::value(GuidToString(inst.task_guid));
            task_ref[std::string_view("pluginGuid")] =
                yyjson::value(GuidToString(inst.plugin_guid));
            const bool has_authoring =
                capability_registry_.FindAuthoring(inst.task_guid) != nullptr;
            task_ref[std::string_view("configurationMode")] =
                has_authoring ? std::string_view("authoring")
                              : std::string_view("properties");
            task_ref[std::string_view("propertiesWritable")] = !has_authoring;
            if (has_authoring && !inst.authoring.is_null())
            {
                task_ref[std::string_view("authoring")] =
                    CloneJsonValue(inst.authoring);
            }

            if (inst.availability == TaskAvailability::Available)
            {
                task_ref[std::string_view("availability")] =
                    std::string_view("available");
            }
            else if (inst.availability == TaskAvailability::Unavailable)
            {
                task_ref[std::string_view("availability")] =
                    std::string_view("unavailable");
                task_ref[std::string_view("unavailabilityReason")] =
                    yyjson::value(inst.unavailability_reason);
            }
            else
            {
                task_ref[std::string_view("availability")] =
                    std::string_view("invalid");
                task_ref[std::string_view("unavailabilityReason")] =
                    yyjson::value(inst.unavailability_reason);
            }

            task_ref[std::string_view("enabled")] = inst.enabled;

            if (inst.next_execution_time)
            {
                task_ref[std::string_view("nextExecutionTime")] =
                    *inst.next_execution_time;
            }
            else
            {
                task_ref[std::string_view("nextExecutionTime")] = nullptr;
            }

            if (!inst.properties.is_null())
            {
                task_ref[std::string_view("properties")] =
                    CloneJsonValue(inst.properties);
            }
            else
            {
                task_ref[std::string_view("properties")] =
                    Das::Utils::MakeYyjsonObject();
            }
            tasks_arr_ref.emplace_back(std::move(task_obj));
        }

        result_obj[std::string_view("tasks")] = std::move(tasks_arr);
        return result;
    }

    yyjson::value SchedulerService::GetTaskRepository()
    {
        std::shared_ptr<TaskRepositoryStore> store;
        {
            const std::scoped_lock lock(mutex_);
            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }
            store = task_repository_store_;
        }

        Repository::Dto::RepositoryGetResponseDto response;
        response.entries = store->ListEntries();
        {
            const std::scoped_lock lock(mutex_);
            for (auto& entry : response.entries)
            {
                entry.availability = DeriveRepositoryAvailabilityLocked(entry);
            }
        }
        return CloneJsonValue(yyjson::object(response));
    }

    yyjson::value SchedulerService::CreateRepositoryEntry(
        const yyjson::value& request)
    {
        Repository::Dto::CreateRepositoryEntryRequestDto create_request;
        if (!ParseCreateRepositoryEntryRequest(request, create_request))
        {
            return MakeRepositoryResponseError(
                "invalidJson",
                "Repository create request must include pluginGuid and "
                "taskTypeGuid strings");
        }

        DasGuid plugin_guid{};
        DasGuid task_guid{};
        try
        {
            plugin_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                create_request.plugin_guid);
            task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                create_request.task_type_guid);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidGuid",
                "Repository create request contains an invalid GUID");
        }

        std::shared_ptr<TaskRepositoryStore> store;
        TaskTypeRecord                       type_snapshot;
        {
            const std::scoped_lock lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return MakeRepositoryResponseError(
                    "taskWorking",
                    "Scheduler is running or stopping");
            }

            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }

            auto* type_record = FindTaskType(task_guid);
            if (!type_record || type_record->plugin_guid != plugin_guid)
            {
                return MakeRepositoryResponseError(
                    "notFound",
                    "Task type was not found in the initialized registry");
            }

            type_snapshot = *type_record;
            store = task_repository_store_;
        }

        Repository::Dto::RepositoryEntryDto entry;
        auto                                create_result = store->CreateEntry(
            create_request,
            type_snapshot.name,
            type_snapshot.descriptors,
            entry);
        if (DAS::IsFailed(create_result))
        {
            return MakeRepositoryResponseError(
                RepositoryErrorKindFromResult(create_result),
                "Failed to create repository entry");
        }

        return CloneJsonValue(yyjson::object(entry));
    }

    DasResult SchedulerService::DeleteRepositoryEntry(int64_t entry_id)
    {
        std::shared_ptr<TaskRepositoryStore> store;
        {
            const std::scoped_lock lock(mutex_);
            if (!initialized_ || !task_repository_store_)
            {
                return DAS_E_OBJECT_NOT_INIT;
            }
            store = task_repository_store_;
        }

        auto result = store->DeleteEntry(entry_id);
        if (DAS::IsFailed(result))
        {
            return result;
        }
        // Entry deleted — drop its cached authoring session so a recreated
        // entry id rebuilds against fresh state.
        {
            const std::scoped_lock lock(mutex_);
            EvictAuthoringSession(repository_authoring_sessions_, entry_id);
        }
        return DAS_S_OK;
    }

    yyjson::value SchedulerService::RenameRepositoryEntry(
        int64_t              entry_id,
        const yyjson::value& request)
    {
        Repository::Dto::RenameRepositoryEntryRequestDto rename_request;
        if (!ParseRenameRepositoryEntryRequest(request, rename_request))
        {
            return MakeRepositoryResponseError(
                "invalidJson",
                "Repository rename request must include displayName string");
        }

        std::shared_ptr<TaskRepositoryStore> store;
        {
            const std::scoped_lock lock(mutex_);
            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }
            store = task_repository_store_;
        }

        Repository::Dto::RepositoryEntryDto entry;
        auto                                rename_result =
            store->RenameEntry(entry_id, rename_request, entry);
        if (DAS::IsFailed(rename_result))
        {
            return MakeRepositoryResponseError(
                RepositoryErrorKindFromResult(rename_result),
                "Failed to rename repository entry");
        }

        {
            const std::scoped_lock lock(mutex_);
            entry.availability = DeriveRepositoryAvailabilityLocked(entry);
        }

        return CloneJsonValue(yyjson::object(entry));
    }

    std::shared_ptr<AuthoringSessionEntry>
    SchedulerService::GetOrCreateAuthoringSession(
        std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>&
                                       cache,
        const TaskAuthoringCapability& capability,
        int64_t                        id,
        yyjson::value                  context,
        int64_t                        current_revision,
        bool                           enforce_fresh)
    {
        return ResolveCachedAuthoringSession(
            cache,
            mutex_,
            plugin_manager_,
            capability,
            id,
            std::move(context),
            current_revision,
            enforce_fresh);
    }

    std::shared_ptr<AuthoringSessionEntry>
    SchedulerService::FindCachedAuthoringSession(
        std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>&
                cache,
        int64_t id)
    {
        const std::scoped_lock lock(mutex_);
        auto                   it = cache.find(id);
        if (it == cache.end())
        {
            return nullptr;
        }
        return it->second;
    }

    void SchedulerService::EvictAuthoringSession(
        std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>&
                cache,
        int64_t id)
    {
        // mutex_ held by caller. Direct erase — no drain. The map's shared_ptr
        // releases its reference, but any in-flight caller that already took a
        // copy keeps the entry (and its session) alive until it finishes
        // (D-87-07 shared_ptr upgrade). That is what roots out the UAF a drain
        // cycle was designed to prevent, so the drain is unnecessary here.
        auto it = cache.find(id);
        if (it == cache.end())
        {
            return;
        }
        cache.erase(it);
    }

    void SchedulerService::InvalidateAuthoringSessions()
    {
        const std::scoped_lock lock(mutex_);
        InvalidateAuthoringSessionsLocked();
    }

    void SchedulerService::InvalidateAuthoringSessionsLocked()
    {
        // No per-entry drain. In-flight callers keep their entries alive via
        // shared_ptr copies (D-87-07), so clearing the maps cannot dangle a
        // session a caller is still using. The caller must hold mutex_.
        task_authoring_sessions_.clear();
        repository_authoring_sessions_.clear();
    }

    yyjson::value SchedulerService::GetRepositoryEntryAuthoringDocument(
        int64_t              entry_id,
        const yyjson::value& request)
    {
        TaskAuthoringCapability                    capability;
        Repository::Dto::RepositoryEntryDto        entry;
        Repository::Dto::RepositoryAvailabilityDto availability;
        {
            const std::scoped_lock lock(mutex_);
            auto                   idle_guard = CheckSchedulerIdleLocked();
            if (idle_guard)
            {
                return MakeRepositoryResponseError(
                    idle_guard->error_kind,
                    idle_guard->message);
            }
            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  entry_json = settings.GetTaskRepositoryEntryJson("0", entry_id);
        if (entry_json.is_null() || !entry_json.is_object())
        {
            return MakeRepositoryResponseError(
                "notFound",
                "Repository entry was not found");
        }

        try
        {
            entry =
                yyjson::cast<Repository::Dto::RepositoryEntryDto>(entry_json);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidJson",
                "Repository entry could not be parsed");
        }

        DasGuid task_guid{};
        try
        {
            task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                entry.task_type_guid);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidGuid",
                "Repository entry contains an invalid task GUID");
        }

        {
            const std::scoped_lock lock(mutex_);
            availability = DeriveRepositoryAvailabilityLocked(entry);
            if (availability.state != "available")
            {
                return MakeRepositoryResponseError(
                    availability.reason.empty() ? "unavailable"
                                                : availability.reason,
                    availability.message.empty()
                        ? "Repository entry is unavailable"
                        : availability.message);
            }

            auto capability_guard =
                ResolveAuthoringCapabilityLocked(task_guid, capability);
            if (capability_guard)
            {
                // Repository-side capability message preserved byte-level
                // (Task path uses the helper's generic message verbatim).
                return MakeRepositoryResponseError(
                    capability_guard->error_kind,
                    "Repository entry task type does not declare authoring "
                    "capability");
            }
        }

        // Envelope guard: the contract requires the server to validate the
        // {kind, payload} envelope before invoking the Provider. The shared
        // Get pipeline owns ⑤ session -> ⑥⑦(GetDocument + parse); this caller
        // owns ①② locks/IO, and ⑩ entryId/taskTypeGuid/revision response
        // assembly.
        auto session_entry = GetOrCreateAuthoringSession(
            repository_authoring_sessions_,
            capability,
            entry_id,
            MakeRepositoryAuthoringContextJson(
                plugin_manager_,
                entry_id,
                entry.accepted_properties,
                entry_json),
            GetAuthoringRevision(entry_json),
            /*enforce_fresh=*/true);
        if (!session_entry)
        {
            return MakeRepositoryResponseError(
                "sessionCreateFailed",
                "Failed to create repository authoring session");
        }
        AuthoringGetPipelineResult pipeline;
        {
            const std::shared_lock<std::shared_mutex> read_lock(
                session_entry->lock);
            pipeline = RunAuthoringGetPipeline(session_entry, request);
        }
        if (!pipeline.ok)
        {
            std::string message = std::move(pipeline.error_message);
            if (pipeline.error_kind == "sessionCreateFailed")
            {
                message = "Failed to create repository authoring session";
            }
            else if (pipeline.error_kind == "providerFailed")
            {
                message =
                    "Repository authoring provider failed to create document";
            }
            return MakeRepositoryResponseError(pipeline.error_kind, message);
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("entryId")] = entry_id;
        (*obj)[std::string_view("taskTypeGuid")] =
            std::string(entry.task_type_guid);
        (*obj)[std::string_view("revision")] = entry.authoring.revision;
        if (pipeline.parsed.has_document)
        {
            (*obj)[std::string_view("document")] =
                std::move(pipeline.parsed.document);
        }
        (*obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        return result;
    }

    yyjson::value SchedulerService::ApplyRepositoryEntryAuthoringChange(
        int64_t              entry_id,
        const yyjson::value& change)
    {
        TaskAuthoringCapability              capability;
        Repository::Dto::RepositoryEntryDto  entry;
        std::shared_ptr<TaskRepositoryStore> store;
        {
            const std::scoped_lock lock(mutex_);
            auto                   idle_guard = CheckSchedulerIdleLocked();
            if (idle_guard)
            {
                return MakeRepositoryResponseError(
                    idle_guard->error_kind,
                    idle_guard->message);
            }
            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }
            store = task_repository_store_;
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  entry_json = settings.GetTaskRepositoryEntryJson("0", entry_id);
        if (entry_json.is_null() || !entry_json.is_object())
        {
            return MakeRepositoryResponseError(
                "notFound",
                "Repository entry was not found");
        }

        try
        {
            entry =
                yyjson::cast<Repository::Dto::RepositoryEntryDto>(entry_json);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidJson",
                "Repository entry could not be parsed");
        }

        DasGuid task_guid{};
        try
        {
            task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                entry.task_type_guid);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidGuid",
                "Repository entry contains an invalid task GUID");
        }

        {
            const std::scoped_lock lock(mutex_);
            auto availability = DeriveRepositoryAvailabilityLocked(entry);
            if (availability.state != "available")
            {
                return MakeRepositoryResponseError(
                    availability.reason.empty() ? "unavailable"
                                                : availability.reason,
                    availability.message.empty()
                        ? "Repository entry is unavailable"
                        : availability.message);
            }

            auto capability_guard =
                ResolveAuthoringCapabilityLocked(task_guid, capability);
            if (capability_guard)
            {
                // Repository-side capability message preserved byte-level
                // (Task path uses the helper's generic message verbatim).
                return MakeRepositoryResponseError(
                    capability_guard->error_kind,
                    "Repository entry task type does not declare authoring "
                    "capability");
            }
        }

        // Envelope guard (③): validate the {kind, payload} envelope before any
        // session resolve so an envelope failure does not create a session
        // (D-85-02c) and does not increment the plugin's
        // authoring_session_count.
        auto validation = ValidateAuthoringChange(change);
        if (!validation.valid)
        {
            return MakeRepositoryResponseError(
                validation.error_kind,
                validation.message);
        }

        // Peek the cache (no create). A hit runs the authoritative revision
        // check inside the per-session write lock below (concurrent Apply —
        // REQ-07a/b). A miss runs an early revision check first so a stale
        // baseRevision never creates a session (D-85-02c, byte-level for
        // SchedulerRepositoryAuthoringApplyStaleBaseRevisionReturnsConflict).
        auto cached_entry = FindCachedAuthoringSession(
            repository_authoring_sessions_,
            entry_id);
        if (!cached_entry)
        {
            const int64_t early_revision = GetAuthoringRevision(entry_json);
            const auto    base_revision = GetBaseRevision(change);
            if (!base_revision || *base_revision != early_revision)
            {
                yyjson::value err = MakeRepositoryResponseError(
                    "revisionConflict",
                    "Repository authoring change baseRevision is stale");
                (*err.as_object())[std::string_view("currentRevision")] =
                    early_revision;
                return err;
            }
        }

        // Resolve the session through the cache (hit: reuse; miss: create).
        auto session_entry = cached_entry
                                 ? cached_entry
                                 : GetOrCreateAuthoringSession(
                                       repository_authoring_sessions_,
                                       capability,
                                       entry_id,
                                       MakeRepositoryAuthoringContextJson(
                                           plugin_manager_,
                                           entry_id,
                                           entry.accepted_properties,
                                           entry_json),
                                       GetAuthoringRevision(entry_json),
                                       /*enforce_fresh=*/false);
        if (!session_entry || !session_entry->session)
        {
            return MakeRepositoryResponseError(
                "sessionCreateFailed",
                "Failed to create repository authoring session");
        }

        // Per-session serialization (D-87-07): unique_lock wraps the
        // authoritative revision check (fresh settings read), Provider
        // ApplyChange (M), and the store persist (W). The M-W runs outside
        // mutex_ and inside session_entry->lock. Lock order mutex_ ->
        // entry->lock is forward (mutex_ never held when entry->lock is taken).
        AuthoringApplyPipelineResult pipeline;
        bool                         persistence_failed = false;
        {
            const std::unique_lock<std::shared_mutex> write_lock(
                session_entry->lock);

            auto fresh_json =
                settings.GetTaskRepositoryEntryJson("0", entry_id);
            if (fresh_json.is_null() || !fresh_json.is_object())
            {
                return MakeRepositoryResponseError(
                    "notFound",
                    "Repository entry was not found");
            }
            pipeline.current_revision = GetAuthoringRevision(fresh_json);
            const auto base_revision = GetBaseRevision(change);
            if (!base_revision || *base_revision != pipeline.current_revision)
            {
                yyjson::value err = MakeRepositoryResponseError(
                    "revisionConflict",
                    "Repository authoring change baseRevision is stale");
                (*err.as_object())[std::string_view("currentRevision")] =
                    pipeline.current_revision;
                return err;
            }

            if (!RunAuthoringApplyPipelineLocked(
                    session_entry,
                    change,
                    fresh_json,
                    pipeline.current_revision,
                    pipeline))
            {
                std::string message = std::move(pipeline.error_message);
                if (pipeline.error_kind == "sessionCreateFailed")
                {
                    message = "Failed to create repository authoring session";
                }
                else if (pipeline.error_kind == "providerFailed")
                {
                    message = "Repository authoring provider rejected change";
                }
                // applyRejected: keep pipeline.error_message (the real
                // diagnostic) and surface the provider's diagnostics array.
                const yyjson::value* diags =
                    (pipeline.error_kind == "applyRejected")
                        ? &pipeline.parsed.diagnostics
                        : nullptr;
                return MakeRepositoryResponseError(
                    pipeline.error_kind,
                    message,
                    diags);
            }

            yyjson::value updated_entry_json;
            auto          persist_result = store->UpdateAuthoring(
                entry_id,
                pipeline.parsed.accepted_properties,
                pipeline.authoring,
                updated_entry_json);
            if (DAS::IsFailed(persist_result))
            {
                persistence_failed = true;
            }
        }

        if (persistence_failed)
        {
            return MakeRepositoryResponseError(
                "persistenceFailed",
                "Failed to persist accepted repository authoring change");
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          result_obj = result.as_object();
        (*result_obj)[std::string_view("ok")] = true;
        (*result_obj)[std::string_view("entryId")] = entry_id;
        (*result_obj)[std::string_view("revision")] = pipeline.next_revision;
        (*result_obj)[std::string_view("acceptedProperties")] =
            std::move(pipeline.parsed.accepted_properties);
        (*result_obj)[std::string_view("document")] =
            std::move(pipeline.parsed.document);
        (*result_obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        return result;
    }

    yyjson::value SchedulerService::CompileRepositoryEntryAuthoring(
        int64_t              entry_id,
        const yyjson::value& request)
    {
        TaskAuthoringCapability             capability;
        Repository::Dto::RepositoryEntryDto entry;
        {
            const std::scoped_lock lock(mutex_);
            auto                   idle_guard = CheckSchedulerIdleLocked();
            if (idle_guard)
            {
                return MakeRepositoryResponseError(
                    idle_guard->error_kind,
                    idle_guard->message);
            }
            if (!initialized_ || !task_repository_store_)
            {
                return MakeRepositoryResponseError(
                    "notInitialized",
                    "Scheduler profile has not been initialized");
            }
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  entry_json = settings.GetTaskRepositoryEntryJson("0", entry_id);
        if (entry_json.is_null() || !entry_json.is_object())
        {
            return MakeRepositoryResponseError(
                "notFound",
                "Repository entry was not found");
        }

        try
        {
            entry =
                yyjson::cast<Repository::Dto::RepositoryEntryDto>(entry_json);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidJson",
                "Repository entry could not be parsed");
        }

        DasGuid task_guid{};
        try
        {
            task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                entry.task_type_guid);
        }
        catch (const std::exception&)
        {
            return MakeRepositoryResponseError(
                "invalidGuid",
                "Repository entry contains an invalid task GUID");
        }

        {
            const std::scoped_lock lock(mutex_);
            auto availability = DeriveRepositoryAvailabilityLocked(entry);
            if (availability.state != "available")
            {
                return MakeRepositoryResponseError(
                    availability.reason.empty() ? "unavailable"
                                                : availability.reason,
                    availability.message.empty()
                        ? "Repository entry is unavailable"
                        : availability.message);
            }

            auto capability_guard =
                ResolveAuthoringCapabilityLocked(task_guid, capability);
            if (capability_guard)
            {
                // Repository-side capability message preserved byte-level
                // (Task path uses the helper's generic message verbatim).
                return MakeRepositoryResponseError(
                    capability_guard->error_kind,
                    "Repository entry task type does not declare authoring "
                    "capability");
            }
        }

        // D-84-08/09: Compile sends {} to Provider and returns {compile,
        // diagnostics}; Scheduler does not rewrap, project, or accept a client
        // document. D-84-09 forbids client properties/document in Compile.
        // The shared Compile pipeline owns ⑤ session -> ⑥⑦(Compile + parse);
        // this caller owns ①② locks/IO, and ⑩ entryId response assembly.
        auto compile_entry = GetOrCreateAuthoringSession(
            repository_authoring_sessions_,
            capability,
            entry_id,
            MakeRepositoryAuthoringContextJson(
                plugin_manager_,
                entry_id,
                entry.accepted_properties,
                entry_json));
        if (!compile_entry)
        {
            return MakeRepositoryResponseError(
                "sessionCreateFailed",
                "Failed to create repository authoring session");
        }
        AuthoringCompilePipelineResult pipeline;
        {
            const std::shared_lock<std::shared_mutex> read_lock(
                compile_entry->lock);
            pipeline = RunAuthoringCompilePipeline(compile_entry, request);
        }
        if (!pipeline.ok)
        {
            std::string message = std::move(pipeline.error_message);
            if (pipeline.error_kind == "sessionCreateFailed")
            {
                message = "Failed to create repository authoring session";
            }
            else if (pipeline.error_kind == "providerFailed")
            {
                message =
                    "Repository authoring provider failed to compile document";
            }
            return MakeRepositoryResponseError(pipeline.error_kind, message);
        }

        // D-84-08/09: Compile response preserves the Provider envelope
        // `{compile, diagnostics}`. canExecute/summary rewrapping was removed;
        // entryId is echoed purely as a routing ack, matching Get/Apply.
        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("entryId")] = entry_id;
        (*obj)[std::string_view("compile")] =
            std::move(pipeline.parsed.compile);
        (*obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        return result;
    }

    RepositoryInvoke::RepositoryInvokeCompileResult
    SchedulerService::ResolveRepositoryInvokeSnapshot(
        const RepositoryInvoke::Dto::RepositoryTaskRefDto& repository_ref)
    {
        return ResolveRepositoryInvokeSnapshot(
            repository_ref,
            RepositoryInvoke::RepositoryInvokeSourceContext{});
    }

    RepositoryInvoke::RepositoryInvokeCompileResult
    SchedulerService::ResolveRepositoryInvokeSnapshot(
        const RepositoryInvoke::Dto::RepositoryTaskRefDto&     repository_ref,
        const RepositoryInvoke::RepositoryInvokeSourceContext& source_context)
    {
        auto fail = [](std::string code, std::string message)
            -> RepositoryInvoke::RepositoryInvokeCompileResult
        {
            RepositoryInvoke::RepositoryInvokeCompileDiagnostic diagnostic;
            diagnostic.code = std::move(code);
            diagnostic.message = std::move(message);

            RepositoryInvoke::RepositoryInvokeCompileResult result;
            result.ok = false;
            result.diagnostics.push_back(std::move(diagnostic));
            return result;
        };

        {
            const std::scoped_lock lock(mutex_);
            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return fail("task-working", "Scheduler is running or stopping");
            }
            if (!initialized_ || !task_repository_store_)
            {
                return fail(
                    "not-initialized",
                    "Scheduler profile has not been initialized");
            }
        }

        RepositoryInvoke::RepositoryInvokeCompileServices services;
        services.load_entry = [this](int64_t entry_id)
            -> std::optional<Repository::Dto::RepositoryEntryDto>
        {
            auto& settings = plugin_manager_.GetSettingsManager();
            auto  entry_json =
                settings.GetTaskRepositoryEntryJson("0", entry_id);
            if (entry_json.is_null() || !entry_json.is_object())
            {
                return std::nullopt;
            }

            Repository::Dto::RepositoryEntryDto entry;
            try
            {
                entry = yyjson::cast<Repository::Dto::RepositoryEntryDto>(
                    entry_json);
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }

            const std::scoped_lock lock(mutex_);
            entry.availability = DeriveRepositoryAvailabilityLocked(entry);
            return entry;
        };
        services.find_execution_component =
            [this](const Repository::Dto::RepositoryEntryDto& entry)
            -> std::optional<std::string>
        {
            DasGuid task_guid{};
            try
            {
                task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                    entry.task_type_guid);
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }

            const std::scoped_lock lock(mutex_);
            auto*                  execution_component =
                capability_registry_.FindExecutionComponent(task_guid);
            if (execution_component == nullptr)
            {
                return std::nullopt;
            }
            return GuidToString(*execution_component);
        };
        services.compile_authoring =
            [this](
                const Repository::Dto::RepositoryEntryDto& entry,
                const yyjson::value&                       request)
            -> RepositoryInvoke::RepositoryInvokeProviderCompileResult
        {
            DasGuid task_guid{};
            try
            {
                task_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                    entry.task_type_guid);
            }
            catch (const std::exception&)
            {
                RepositoryInvoke::RepositoryInvokeProviderCompileResult result;
                result.ok = false;
                return result;
            }

            TaskAuthoringCapability capability;
            {
                const std::scoped_lock lock(mutex_);
                auto* authoring = capability_registry_.FindAuthoring(task_guid);
                if (authoring == nullptr)
                {
                    RepositoryInvoke::RepositoryInvokeProviderCompileResult
                        result;
                    result.ok = false;
                    return result;
                }
                capability = *authoring;
            }

            auto entry_json = CloneJsonValue(yyjson::object(entry));
            auto compile_entry = GetOrCreateAuthoringSession(
                repository_authoring_sessions_,
                capability,
                entry.entry_id,
                MakeRepositoryAuthoringContextJson(
                    plugin_manager_,
                    entry.entry_id,
                    entry.accepted_properties,
                    entry_json));
            if (!compile_entry)
            {
                RepositoryInvoke::RepositoryInvokeProviderCompileResult result;
                result.ok = false;
                RepositoryInvoke::RepositoryInvokeCompileDiagnostic diagnostic;
                diagnostic.code = "provider-compile-failed";
                diagnostic.message =
                    "Failed to create repository authoring session";
                result.diagnostics.push_back(std::move(diagnostic));
                return result;
            }
            RepositoryProviderCompileResult provider_compile;
            {
                const std::shared_lock<std::shared_mutex> read_lock(
                    compile_entry->lock);
                provider_compile =
                    CompileRepositoryEntryProvider(compile_entry, request);
            }

            RepositoryInvoke::RepositoryInvokeProviderCompileResult result;
            result.ok = provider_compile.Succeeded();
            if (!result.ok)
            {
                RepositoryInvoke::RepositoryInvokeCompileDiagnostic diagnostic;
                diagnostic.code = "provider-compile-failed";
                diagnostic.message = provider_compile.message;
                result.diagnostics.push_back(std::move(diagnostic));
                return result;
            }

            // Phase 84 strict Compile result envelope is `{compile,
            // diagnostics}`. RepositoryInvokeCompiler consumes the inner
            // compile payload, so unwrap the validated envelope here rather
            // than asking every downstream consumer to understand the new wire.
            auto parsed =
                ParseAuthoringProviderCompile(provider_compile.compile);
            if (!parsed.error_kind.empty())
            {
                result.ok = false;
                RepositoryInvoke::RepositoryInvokeCompileDiagnostic diagnostic;
                diagnostic.code = "provider-compile-failed";
                diagnostic.message = parsed.error_message;
                result.diagnostics.push_back(std::move(diagnostic));
                return result;
            }
            result.compile_result = std::move(parsed.compile);
            return result;
        };

        return RepositoryInvoke::ResolveRepositoryInvokeSnapshot(
            services,
            repository_ref,
            source_context);
    }

    // ----------------------------------------------------------------
    // AddTask
    // ----------------------------------------------------------------

    DasResult SchedulerService::AddTask(
        const DasGuid& task_guid,
        int64_t*       out_task_id)
    {
        if (out_task_id == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        *out_task_id = 0;

        // Phase 1: Short lock for validation — copy type data to avoid
        // dangling pointer if Initialize/Disable clears task_types_.
        TaskTypeRecord type_snapshot;
        {
            const std::scoped_lock lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return DAS_E_TASK_WORKING;
            }

            if (!initialized_)
            {
                return DAS_E_OBJECT_NOT_INIT;
            }

            auto* type_rec = FindTaskType(task_guid);
            if (!type_rec)
            {
                return DAS_E_NOT_FOUND;
            }

            type_snapshot = *type_rec;
        } // lock released

        // Phase 2: Lock-free persistence — all SettingsManager calls
        // and file I/O execute outside mutex_. Uses type_snapshot
        // (value copy) so no dangling pointer risk.
        auto&   settings = plugin_manager_.GetSettingsManager();
        auto    scheduler_index = settings.GetSchedulerIndexJson("0");
        int64_t next_id = 0;
        {
            auto si_obj = scheduler_index.as_object();
            if (si_obj)
            {
                auto nid_val =
                    si_obj->operator[](std::string_view("nextTaskId"));
                auto nid = nid_val.as_sint();
                if (nid)
                {
                    next_id = *nid;
                }
            }
        }

        // Build instance with descriptor defaults
        TaskInstanceRecord rec;
        rec.id = next_id;
        rec.task_guid = task_guid;
        rec.plugin_guid = type_snapshot.plugin_guid;
        rec.availability = TaskAvailability::Available;
        rec.properties = Das::Utils::MakeYyjsonObject();
        auto create_result =
            CreateTaskInstance(type_snapshot, rec.task_instance.Put());
        if (IsFailed(create_result) || !rec.task_instance)
        {
            return IsFailed(create_result) ? create_result : DAS_E_FAIL;
        }

        // Initialize properties from descriptor defaults
        {
            auto props_obj = rec.properties.as_object();
            for (const auto& desc : type_snapshot.descriptors)
            {
                if (!std::holds_alternative<std::monostate>(desc.default_value))
                {
                    std::visit(
                        [&props_obj, &desc](const auto& val)
                        {
                            if constexpr (!std::is_same_v<
                                              std::decay_t<decltype(val)>,
                                              std::monostate>)
                            {
                                auto property = props_obj->operator[](
                                    std::string_view(desc.name));
                                if constexpr (
                                    std::is_same_v<
                                        std::decay_t<decltype(val)>,
                                        std::string>)
                                {
                                    property = std::string(val);
                                }
                                else
                                {
                                    property = val;
                                }
                            }
                        },
                        desc.default_value);
                }
            }
        }

        // Persist task instance file
        auto instance_json = Das::Utils::MakeYyjsonObject();
        auto inst_obj = *instance_json.as_object();
        inst_obj[std::string_view("id")] = rec.id;
        inst_obj[std::string_view("taskGuid")] =
            yyjson::value(GuidToString(rec.task_guid));
        inst_obj[std::string_view("pluginGuid")] =
            yyjson::value(GuidToString(rec.plugin_guid));
        inst_obj[std::string_view("nextExecutionTime")] = nullptr;
        inst_obj[std::string_view("enabled")] = true;

        if (!rec.properties.is_null())
        {
            inst_obj[std::string_view("properties")] =
                CloneJsonValue(rec.properties);
        }
        else
        {
            inst_obj[std::string_view("properties")] =
                Das::Utils::MakeYyjsonObject();
        }

        auto persist_result =
            settings.UpdateTaskInstanceJson("0", rec.id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Update scheduler index
        {
            auto si_obj = scheduler_index.as_object();
            if (si_obj)
            {
                si_obj->operator[](std::string_view("nextTaskId")) =
                    next_id + 1;
                auto order_arr =
                    si_obj->operator[](std::string_view("taskOrder"))
                        .as_array();
                if (order_arr)
                {
                    order_arr->emplace_back(next_id);
                }
            }
        }
        auto index_result =
            settings.UpdateSchedulerIndexJson("0", scheduler_index);
        if (IsFailed(index_result))
        {
            // Rollback task instance file (best-effort)
            settings.DeleteTaskInstanceJson("0", rec.id);
            return index_result;
        }

        // Phase 3: Short lock to commit to runtime state
        {
            const std::scoped_lock lock(mutex_);
            auto*                  type_rec = FindTaskType(task_guid);
            rec.task_type = type_rec;
            task_instances_.push_back(std::move(rec));
        }

        *out_task_id = next_id;
        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // DeleteTask
    // ----------------------------------------------------------------

    DasResult SchedulerService::DeleteTask(int64_t task_id)
    {
        // Phase 1: Short lock to validate state and check existence
        {
            const std::scoped_lock lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return DAS_E_TASK_WORKING;
            }

            auto it = std::find_if(
                task_instances_.begin(),
                task_instances_.end(),
                [task_id](const TaskInstanceRecord& r)
                { return r.id == task_id; });

            if (it == task_instances_.end())
            {
                return DAS_E_NOT_FOUND;
            }
        } // lock released — Config side validation done

        // Phase 2: Lock-free SettingsManager persistence with retry.
        // All file I/O and sleep_for execute outside mutex_.
        auto& settings = plugin_manager_.GetSettingsManager();

        auto scheduler_index = settings.GetSchedulerIndexJson("0");
        {
            auto si_obj = scheduler_index.as_object();
            if (si_obj && si_obj->contains(std::string_view("taskOrder"))
                && si_obj->operator[](std::string_view("taskOrder")).is_array())
            {
                auto order_arr =
                    *si_obj->operator[](std::string_view("taskOrder"))
                         .as_array();
                // Build new array without the deleted task
                auto new_arr = Das::Utils::MakeYyjsonArray();
                auto new_arr_ref = *new_arr.as_array();
                for (auto&& oit : order_arr)
                {
                    if (!oit.is_int() || static_cast<int64_t>(oit) != task_id)
                    {
                        new_arr_ref.emplace_back(CloneJsonValue(oit));
                    }
                }
                si_obj->operator[](std::string_view("taskOrder")) =
                    std::move(new_arr);
            }
        }

        constexpr int MAX_RETRIES = 3;
        constexpr int BASE_DELAY_MS = 100;
        DasResult     last_error = DAS_S_OK;

        for (int attempt = 0; attempt < MAX_RETRIES; ++attempt)
        {
            if (attempt > 0)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(BASE_DELAY_MS << (attempt - 1)));
            }

            auto delete_result = settings.DeleteTaskInstanceJson("0", task_id);
            auto index_result =
                settings.UpdateSchedulerIndexJson("0", scheduler_index);

            if (!DAS::IsFailed(delete_result) && !DAS::IsFailed(index_result))
            {
                // Persistence succeeded — remove from runtime state
                const std::scoped_lock commit_lock(mutex_);
                auto                   it = std::find_if(
                    task_instances_.begin(),
                    task_instances_.end(),
                    [task_id](const TaskInstanceRecord& r)
                    { return r.id == task_id; });
                if (it != task_instances_.end())
                {
                    task_instances_.erase(it);
                }
                // Drop the cached authoring session so a later re-created task
                // id rebuilds against fresh state. commit_lock holds mutex_;
                // EvictAuthoringSession assumes the caller holds it.
                EvictAuthoringSession(task_authoring_sessions_, task_id);
                return DAS_S_OK;
            }

            last_error =
                DAS::IsFailed(delete_result) ? delete_result : index_result;
        }

        // All retries failed: runtime state unchanged (task remains in
        // task_instances_), no rollback needed.
        DAS_CORE_LOG_ERROR(
            "SchedulerService::DeleteTask: persistence failed for task {} "
            "after {} retries, result={}",
            task_id,
            MAX_RETRIES,
            last_error);

        return last_error;
    }

    // ----------------------------------------------------------------
    // UpdateTaskProperties
    // ----------------------------------------------------------------

    DasResult SchedulerService::UpdateTaskProperties(
        int64_t              task_id,
        const yyjson::value& properties)
    {
        // Phase 1: Short lock for validation and snapshot — copy
        // descriptors to avoid dangling pointer across lock-free phases.
        std::vector<Das::Core::ForeignInterfaceHost::PluginSettingDesc>
                      descriptors;
        yyjson::value current_properties;
        {
            const std::scoped_lock lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return DAS_E_TASK_WORKING;
            }

            auto* inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return DAS_E_NOT_FOUND;
            }

            // Invalid/unavailable instances cannot have properties updated
            if (inst->availability != TaskAvailability::Available)
            {
                return DAS_E_FAIL;
            }

            if (!inst->task_type)
            {
                return DAS_E_FAIL;
            }

            if (capability_registry_.FindAuthoring(inst->task_guid) != nullptr)
            {
                return DAS_E_ACCESS_DENIED;
            }

            descriptors = inst->task_type->descriptors;
            if (!inst->properties.is_null())
            {
                current_properties = CloneJsonValue(inst->properties);
            }
            else
            {
                current_properties = Das::Utils::MakeYyjsonObject();
            }
        } // lock released

        if (!properties.is_object())
        {
            return DAS_E_INVALID_JSON;
        }

        // Validate property names and types against descriptors
        auto props_obj = properties.as_object();
        for (auto it = props_obj->begin(); it != props_obj->end(); ++it)
        {
            const auto& prop_name = it->first;
            const auto& prop_val = it->second;
            bool        found = false;
            for (const auto& desc : descriptors)
            {
                if (desc.name == std::string_view(prop_name))
                {
                    // Type validation
                    switch (desc.type)
                    {
                    case Das::ExportInterface::DAS_TYPE_BOOL:
                        if (!prop_val.is_bool())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_INT:
                        if (!prop_val.is_int())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_FLOAT:
                        if (!prop_val.is_num())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_STRING:
                        if (!prop_val.is_string())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    default:
                        break;
                    }
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return DAS_E_NOT_FOUND;
            }
        }

        // Apply validated properties to snapshot (mutable copy)
        {
            auto cur_obj = current_properties.as_object();
            for (auto it = props_obj->begin(); it != props_obj->end(); ++it)
            {
                const auto& prop_name = it->first;
                const auto& prop_val = it->second;
                cur_obj->operator[](std::string_view(prop_name)) =
                    CloneJsonValue(prop_val);
            }
        }

        // Phase 2: Lock-free SettingsManager persistence
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  instance_json = settings.GetTaskInstanceJson("0", task_id);
        if (instance_json.is_null() || !instance_json.is_object())
        {
            instance_json = Das::Utils::MakeYyjsonObject();
        }
        {
            auto inst_obj = instance_json.as_object();
            inst_obj->operator[](std::string_view("properties")) =
                CloneJsonValue(current_properties);
        }
        auto persist_result =
            settings.UpdateTaskInstanceJson("0", task_id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Phase 3: Short lock to commit properties to runtime state
        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (inst)
            {
                inst->properties = std::move(current_properties);
            }
        }

        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // UpdateTaskInternalProperties
    // ----------------------------------------------------------------

    DasResult SchedulerService::UpdateTaskInternalProperties(
        int64_t              task_id,
        const yyjson::value& internal_props)
    {
        // Phase 1: Short lock for validation and snapshot
        std::optional<int64_t> new_next_time;
        bool                   has_update = false;
        {
            const std::scoped_lock lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return DAS_E_TASK_WORKING;
            }

            auto* inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return DAS_E_NOT_FOUND;
            }

            // Parse nextExecutionTime if provided
            auto ip_obj = internal_props.as_object();
            auto net_key = std::string_view("nextExecutionTime");
            if (ip_obj && ip_obj->contains(net_key))
            {
                const auto& val = ip_obj->operator[](net_key);
                if (val.is_null())
                {
                    new_next_time = std::nullopt;
                    has_update = true;
                }
                else if (val.is_int())
                {
                    auto net_int = val.as_sint();
                    if (net_int)
                    {
                        new_next_time = *net_int;
                        has_update = true;
                    }
                    else
                    {
                        return DAS_E_TYPE_ERROR;
                    }
                }
                else
                {
                    return DAS_E_TYPE_ERROR;
                }
            }
        } // lock released

        if (!has_update)
        {
            return DAS_S_OK;
        }

        // Phase 2: Lock-free SettingsManager persistence
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  instance_json = settings.GetTaskInstanceJson("0", task_id);
        if (instance_json.is_null() || !instance_json.is_object())
        {
            instance_json = Das::Utils::MakeYyjsonObject();
        }
        {
            auto inst_obj = instance_json.as_object();
            if (new_next_time.has_value())
            {
                inst_obj->operator[](std::string_view("nextExecutionTime")) =
                    *new_next_time;
            }
            else
            {
                inst_obj->operator[](std::string_view("nextExecutionTime")) =
                    nullptr;
            }
        }
        auto persist_result =
            settings.UpdateTaskInstanceJson("0", task_id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Phase 3: Short lock to commit to runtime state
        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (inst)
            {
                inst->next_execution_time = new_next_time;
            }
        }

        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // SetTaskEnabled
    // ----------------------------------------------------------------

    DasResult SchedulerService::SetTaskEnabled(int64_t task_id, bool enabled)
    {
        // Phase 1: Short lock to validate existence and capture current value.
        // No Running/Stopping guard: toggling enabled is a runtime-safe intent
        // switch — the dispatch loop reads the flag on the next tick, so it is
        // safe to flip while the scheduler is running (that is the feature's
        // core value: pause/resume a single task without stopping the world).
        {
            const std::scoped_lock lock(mutex_);

            if (!initialized_)
            {
                return DAS_E_OBJECT_NOT_INIT;
            }

            auto* inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return DAS_E_NOT_FOUND;
            }

            if (inst->enabled == enabled)
            {
                // No-op: avoid a redundant persistence write.
                return DAS_S_OK;
            }
        }

        // Phase 2: Lock-free SettingsManager persistence. Read-modify-write
        // the persisted instance so the toggle survives restart.
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  instance_json = settings.GetTaskInstanceJson("0", task_id);
        if (instance_json.is_null() || !instance_json.is_object())
        {
            instance_json = Das::Utils::MakeYyjsonObject();
        }
        {
            auto inst_obj = instance_json.as_object();
            inst_obj->operator[](std::string_view("enabled")) = enabled;
        }
        auto persist_result =
            settings.UpdateTaskInstanceJson("0", task_id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Phase 3: Short lock to commit to runtime state.
        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (inst)
            {
                inst->enabled = enabled;
            }
        }

        return DAS_S_OK;
    }

    yyjson::value SchedulerService::GetTaskAuthoringDocument(
        int64_t              task_id,
        const yyjson::value& request)
    {
        TaskAuthoringCapability capability;
        DasGuid                 task_guid{};
        yyjson::value           properties;
        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return MakeAuthoringResponseError(
                    "notFound",
                    "Task instance was not found");
            }
            if (inst->availability != TaskAvailability::Available)
            {
                return MakeAuthoringResponseError(
                    "unavailable",
                    "Task instance is unavailable");
            }
            auto capability_guard =
                ResolveAuthoringCapabilityLocked(inst->task_guid, capability);
            if (capability_guard)
            {
                return MakeAuthoringResponseError(
                    capability_guard->error_kind,
                    capability_guard->message);
            }
            task_guid = inst->task_guid;
            properties = CloneJsonValue(inst->properties);
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  task_json = settings.GetTaskInstanceJson("0", task_id);

        // Envelope guard: the contract requires the server to validate the
        // {kind, payload} envelope before invoking the Provider. The shared
        // Get pipeline owns ⑤ session -> ⑥⑦(GetDocument + parse); this caller
        // owns ①② lock/IO, and ⑩ taskId response assembly.
        auto entry = GetOrCreateAuthoringSession(
            task_authoring_sessions_,
            capability,
            task_id,
            MakeAuthoringContextJson(
                plugin_manager_,
                task_id,
                properties,
                task_json),
            GetAuthoringRevision(task_json),
            /*enforce_fresh=*/true);
        if (!entry)
        {
            return MakeAuthoringResponseError(
                "sessionCreateFailed",
                "Failed to create task authoring session");
        }
        AuthoringGetPipelineResult pipeline;
        {
            const std::shared_lock<std::shared_mutex> read_lock(entry->lock);
            pipeline = RunAuthoringGetPipeline(entry, request);
        }
        if (!pipeline.ok)
        {
            std::string message = std::move(pipeline.error_message);
            if (pipeline.error_kind == "sessionCreateFailed")
            {
                message = "Failed to create task authoring session";
            }
            else if (pipeline.error_kind == "providerFailed")
            {
                message = "Task authoring provider failed to create document";
            }
            return MakeAuthoringResponseError(pipeline.error_kind, message);
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("taskId")] = task_id;
        (*obj)[std::string_view("taskGuid")] = GuidToString(task_guid);
        if (pipeline.parsed.has_document)
        {
            (*obj)[std::string_view("document")] =
                std::move(pipeline.parsed.document);
        }
        (*obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        return result;
    }

    yyjson::value SchedulerService::ApplyTaskAuthoringChange(
        int64_t              task_id,
        const yyjson::value& change)
    {
        TaskAuthoringCapability capability;
        yyjson::value           properties;
        {
            const std::scoped_lock lock(mutex_);
            auto                   idle_guard = CheckSchedulerIdleLocked();
            if (idle_guard)
            {
                return MakeAuthoringResponseError(
                    idle_guard->error_kind,
                    idle_guard->message);
            }

            auto* inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return MakeAuthoringResponseError(
                    "notFound",
                    "Task instance was not found");
            }
            if (inst->availability != TaskAvailability::Available)
            {
                return MakeAuthoringResponseError(
                    "unavailable",
                    "Task instance is unavailable");
            }
            auto capability_guard =
                ResolveAuthoringCapabilityLocked(inst->task_guid, capability);
            if (capability_guard)
            {
                return MakeAuthoringResponseError(
                    capability_guard->error_kind,
                    capability_guard->message);
            }
            properties = CloneJsonValue(inst->properties);
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  task_json = settings.GetTaskInstanceJson("0", task_id);
        if (task_json.is_null() || !task_json.is_object())
        {
            return MakeAuthoringResponseError(
                "notFound",
                "Task instance file was not found");
        }

        // Envelope guard (③): validate the {kind, payload} envelope before any
        // session resolve so an envelope failure does not create a session
        // (D-85-02c) and does not increment the plugin's
        // authoring_session_count.
        auto validation = ValidateAuthoringChange(change);
        if (!validation.valid)
        {
            return MakeAuthoringResponseError(
                validation.error_kind,
                validation.message);
        }

        // Peek the cache (no create). A hit means a session already exists, so
        // the authoritative revision check runs inside the per-session write
        // lock below (handles concurrent Apply on the same task — REQ-07a/b). A
        // miss runs an early revision check first so a stale baseRevision never
        // creates a session (D-85-02c, preserved byte-level for the
        // revisionConflict regression cases).
        auto cached_entry =
            FindCachedAuthoringSession(task_authoring_sessions_, task_id);
        if (!cached_entry)
        {
            const int64_t early_revision = GetAuthoringRevision(task_json);
            const auto    base_revision = GetBaseRevision(change);
            if (!base_revision || *base_revision != early_revision)
            {
                yyjson::value err = MakeAuthoringResponseError(
                    "revisionConflict",
                    "Authoring change baseRevision is stale");
                (*err.as_object())[std::string_view("currentRevision")] =
                    early_revision;
                return err;
            }
        }

        // Resolve the session through the cache (hit: reuse; miss: create — the
        // early check above already passed). Apply passes enforce_fresh=false
        // so consecutive Applies share one session (D-87-06 cache reuse).
        auto entry = cached_entry ? cached_entry
                                  : GetOrCreateAuthoringSession(
                                        task_authoring_sessions_,
                                        capability,
                                        task_id,
                                        MakeAuthoringContextJson(
                                            plugin_manager_,
                                            task_id,
                                            properties,
                                            task_json),
                                        GetAuthoringRevision(task_json),
                                        /*enforce_fresh=*/false);
        if (!entry || !entry->session)
        {
            return MakeAuthoringResponseError(
                "sessionCreateFailed",
                "Failed to create task authoring session");
        }

        // Per-session serialization (D-87-07): the unique_lock wraps the
        // authoritative revision check (on a fresh settings read), the Provider
        // ApplyChange (M), and the settings persist (W). The whole M-W runs
        // outside mutex_ and inside entry->lock; mutex_ is re-acquired only for
        // the short in-memory inst metadata update after the lock releases.
        // Lock order is mutex_ -> entry->lock (forward): mutex_ is never held
        // when entry->lock is acquired, so there is no AB-BA with the Get path
        // (which also never holds them simultaneously).
        AuthoringApplyPipelineResult pipeline;
        bool                         persistence_failed = false;
        {
            const std::unique_lock<std::shared_mutex> write_lock(entry->lock);

            auto fresh_json = settings.GetTaskInstanceJson("0", task_id);
            if (fresh_json.is_null() || !fresh_json.is_object())
            {
                return MakeAuthoringResponseError(
                    "notFound",
                    "Task instance file was not found");
            }
            pipeline.current_revision = GetAuthoringRevision(fresh_json);
            const auto base_revision = GetBaseRevision(change);
            if (!base_revision || *base_revision != pipeline.current_revision)
            {
                yyjson::value err = MakeAuthoringResponseError(
                    "revisionConflict",
                    "Authoring change baseRevision is stale");
                (*err.as_object())[std::string_view("currentRevision")] =
                    pipeline.current_revision;
                return err;
            }

            if (!RunAuthoringApplyPipelineLocked(
                    entry,
                    change,
                    fresh_json,
                    pipeline.current_revision,
                    pipeline))
            {
                std::string message = std::move(pipeline.error_message);
                if (pipeline.error_kind == "providerFailed")
                {
                    message = "Task authoring provider rejected change";
                }
                else if (pipeline.error_kind == "sessionCreateFailed")
                {
                    message = "Failed to create task authoring session";
                }
                // applyRejected: keep pipeline.error_message (the real
                // diagnostic); also surface the provider's diagnostics array so
                // the UI shows the precise rejection reason.
                const yyjson::value* diags =
                    (pipeline.error_kind == "applyRejected")
                        ? &pipeline.parsed.diagnostics
                        : nullptr;
                return MakeAuthoringResponseError(
                    pipeline.error_kind,
                    message,
                    diags);
            }

            auto fresh_obj = fresh_json.as_object();
            (*fresh_obj)[std::string_view("properties")] =
                CloneJsonValue(pipeline.parsed.accepted_properties);
            (*fresh_obj)[std::string_view("authoring")] =
                CloneJsonValue(pipeline.authoring);
            if (DAS::IsFailed(
                    settings.UpdateTaskInstanceJson("0", task_id, fresh_json)))
            {
                persistence_failed = true;
            }
        }

        if (persistence_failed)
        {
            return MakeAuthoringResponseError(
                "persistenceFailed",
                "Failed to persist accepted authoring change");
        }

        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (inst)
            {
                inst->properties =
                    CloneJsonValue(pipeline.parsed.accepted_properties);
                inst->authoring = CloneJsonValue(pipeline.authoring);
            }
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          result_obj = result.as_object();
        (*result_obj)[std::string_view("ok")] = true;
        (*result_obj)[std::string_view("taskId")] = task_id;
        (*result_obj)[std::string_view("revision")] = pipeline.next_revision;
        (*result_obj)[std::string_view("acceptedProperties")] =
            std::move(pipeline.parsed.accepted_properties);
        (*result_obj)[std::string_view("document")] =
            std::move(pipeline.parsed.document);
        (*result_obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        return result;
    }

    yyjson::value SchedulerService::CompileTaskAuthoring(
        int64_t              task_id,
        const yyjson::value& request)
    {
        TaskAuthoringCapability capability;
        yyjson::value           properties;
        {
            const std::scoped_lock lock(mutex_);
            auto*                  inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return MakeAuthoringResponseError(
                    "notFound",
                    "Task instance was not found");
            }
            auto capability_guard =
                ResolveAuthoringCapabilityLocked(inst->task_guid, capability);
            if (capability_guard)
            {
                return MakeAuthoringResponseError(
                    capability_guard->error_kind,
                    capability_guard->message);
            }
            properties = CloneJsonValue(inst->properties);
        }

        auto& settings = plugin_manager_.GetSettingsManager();
        auto  task_json = settings.GetTaskInstanceJson("0", task_id);

        // Envelope guard: the contract requires the server to validate the
        // {kind, payload} envelope before invoking the Provider. The shared
        // Compile pipeline owns ⑤ session -> ⑥⑦(Compile + parse); this caller
        // owns ①② lock/IO, and ⑩ taskId response assembly.
        auto compile_entry = GetOrCreateAuthoringSession(
            task_authoring_sessions_,
            capability,
            task_id,
            MakeAuthoringContextJson(
                plugin_manager_,
                task_id,
                properties,
                task_json));
        if (!compile_entry)
        {
            return MakeAuthoringResponseError(
                "sessionCreateFailed",
                "Failed to create task authoring session");
        }
        AuthoringCompilePipelineResult pipeline;
        {
            const std::shared_lock<std::shared_mutex> read_lock(
                compile_entry->lock);
            pipeline = RunAuthoringCompilePipeline(compile_entry, request);
        }
        if (!pipeline.ok)
        {
            std::string message = std::move(pipeline.error_message);
            if (pipeline.error_kind == "sessionCreateFailed")
            {
                message = "Failed to create task authoring session";
            }
            else if (pipeline.error_kind == "providerFailed")
            {
                message = "Task authoring provider failed to compile document";
            }
            return MakeAuthoringResponseError(pipeline.error_kind, message);
        }

        yyjson::value result(Das::Utils::MakeYyjsonObject());
        auto          obj = result.as_object();
        (*obj)[std::string_view("taskId")] = task_id;
        (*obj)[std::string_view("compile")] =
            std::move(pipeline.parsed.compile);
        (*obj)[std::string_view("diagnostics")] =
            std::move(pipeline.parsed.diagnostics);
        return result;
    }

    // ----------------------------------------------------------------
    // Timer and tick
    // ----------------------------------------------------------------

    void SchedulerService::StartTickTimer(
        std::chrono::steady_clock::duration delay)
    {
        if (!tick_timer_)
        {
            return;
        }

        tick_timer_->expires_after(delay);
        tick_timer_->async_wait(
            [this](const boost::system::error_code& ec)
            {
                if (!ec && state_.load() == SchedulerState::Running)
                {
                    auto* callback = new TickCallback(this);
                    ipc_context_.get().PostToBusinessThread(callback);
                    callback->Release();
                }
            });
    }

    void SchedulerService::OnTick()
    {
        if (state_.load() != SchedulerState::Running)
        {
            return;
        }

        // Select a runnable task instance under lock, then release lock
        // before calling plugin Do.
        Das::PluginInterface::IDasTask*        task_ptr = nullptr;
        TaskInstanceRecord*                    selected_inst = nullptr;
        yyjson::value                          properties_copy;
        std::optional<TaskAuthoringCapability> authoring_capability;
        int64_t                                selected_id = -1;
        std::chrono::steady_clock::duration    next_delay =
            std::chrono::seconds(1);

        int64_t now_unix = SystemClock::to_time_t(SystemClock::now());

        {
            const std::scoped_lock lock(mutex_);

            if (state_.load() != SchedulerState::Running)
            {
                return;
            }

            selected_inst = FindNextRunnableTask(task_instances_, now_unix);

            if (!selected_inst)
            {
                DAS_CORE_LOG_DEBUG(
                    "SchedulerService::OnTick: no runnable task");
                next_delay = ComputeNextDelay(task_instances_, now_unix);
                StartTickTimer(next_delay);
                return;
            }

            task_ptr = selected_inst->task_instance.Get();
            properties_copy = selected_inst->properties;
            selected_id = selected_inst->id;
            if (auto* authoring = capability_registry_.FindAuthoring(
                    selected_inst->task_guid))
            {
                authoring_capability = *authoring;
            }

            current_task_ = task_ptr;

            // Create or reuse stop token for cooperative cancellation
            if (!stop_token_)
            {
                stop_token_ = Das::Core::Utils::DasStopTokenImpl::Make();
            }
        }

        // Create IDasJson inputs outside the lock (no serialization round-trip)
        DasPtr<Das::ExportInterface::IDasJson> p_env_json;
        auto env_cr = CreateEmptyDasJson(p_env_json.Put());
        if (IsFailed(env_cr))
        {
            p_env_json.Reset();
        }

        DasPtr<Das::ExportInterface::IDasJson> p_props_json;
        bool                                   compile_failed = false;
        if (authoring_capability)
        {
            auto task_json =
                plugin_manager_.GetSettingsManager().GetTaskInstanceJson(
                    "0",
                    selected_id);

            auto ontick_entry = GetOrCreateAuthoringSession(
                task_authoring_sessions_,
                *authoring_capability,
                selected_id,
                MakeAuthoringContextJson(
                    plugin_manager_,
                    selected_id,
                    properties_copy,
                    task_json));
            if (!ontick_entry || !ontick_entry->session)
            {
                compile_failed = true;
                DAS_CORE_LOG_WARN(
                    "SchedulerService::OnTick: task {} authoring session "
                    "create failed",
                    selected_id);
            }
            else
            {
                const std::shared_lock<std::shared_mutex> read_lock(
                    ontick_entry->lock);
                auto& session = ontick_entry->session;
                auto  request_json =
                    WrapJsonValue(MakeExecutionCompileRequest());
                DasPtr<Das::ExportInterface::IDasJson> compile_json;
                auto                                   compile_result =
                    session->Compile(request_json.Get(), compile_json.Put());
                if (DAS::IsFailed(compile_result) || !compile_json)
                {
                    compile_failed = true;
                    DAS_CORE_LOG_WARN(
                        "SchedulerService::OnTick: task {} authoring compile "
                        "failed, result={}",
                        selected_id,
                        compile_result);
                }
                else
                {
                    auto compiled = ReadJsonInterface(compile_json.Get());
                    auto parsed_compile =
                        ParseAuthoringProviderCompile(compiled);
                    if (!parsed_compile.error_kind.empty())
                    {
                        compile_failed = true;
                        DAS_CORE_LOG_WARN(
                            "SchedulerService::OnTick: task {} authoring "
                            "compile result rejected: {}",
                            selected_id,
                            parsed_compile.error_kind);
                    }
                    else if (
                        auto compile_obj = parsed_compile.compile.as_object();
                        compile_obj
                        && IsExplicitFalse(
                            (*compile_obj)[std::string_view("ok")]))
                    {
                        compile_failed = true;
                        DAS_CORE_LOG_WARN(
                            "SchedulerService::OnTick: task {} authoring "
                            "compile returned ok=false",
                            selected_id);
                    }
                    else
                    {
                        try
                        {
                            p_props_json =
                                Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
                                    ExtractProviderExecutionInput(
                                        parsed_compile.compile));
                        }
                        catch (const std::bad_alloc&)
                        {
                            compile_failed = true;
                            DAS_CORE_LOG_WARN(
                                "Failed to allocate IDasJson for compiled "
                                "task input");
                        }
                    }
                }
            }
        }
        else
        {
            try
            {
                p_props_json = Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
                    std::move(properties_copy));
            }
            catch (const std::bad_alloc&)
            {
                DAS_CORE_LOG_WARN(
                    "Failed to allocate IDasJson for task properties");
            }
        }

        if (!compile_failed)
        {
            // Call IDasTask::Do WITHOUT holding the mutex
            auto do_result = task_ptr->Do(
                stop_token_.Get(),
                p_env_json ? p_env_json.Get() : nullptr,
                p_props_json ? p_props_json.Get() : nullptr);

            if (IsFailed(do_result))
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::OnTick: task {} Do returned result={}",
                    selected_id,
                    do_result);
            }
        }

        // Refresh nextExecutionTime from the task
        int64_t                       refreshed_time = 0;
        bool                          has_refreshed_time = false;
        Das::ExportInterface::DasDate next_date{};
        if (compile_failed)
        {
            refreshed_time = SENTINEL_FUTURE_TIMESTAMP;
            has_refreshed_time = true;
            DAS_CORE_LOG_WARN(
                "SchedulerService::OnTick: task {} authoring compile failed, "
                "setting sentinel nextExecutionTime={}",
                selected_id,
                SENTINEL_FUTURE_TIMESTAMP);
        }
        else
        {
            auto net_result = task_ptr->GetNextExecutionTime(&next_date);
            if (IsOk(net_result))
            {
                refreshed_time = DasDateToUnix(next_date);
                has_refreshed_time = true;
            }
            else
            {
                // GetNextExecutionTime failed: use sentinel to prevent hot loop
                refreshed_time = SENTINEL_FUTURE_TIMESTAMP;
                has_refreshed_time = true;
                DAS_CORE_LOG_WARN(
                    "SchedulerService::OnTick: task {} GetNextExecutionTime "
                    "failed (result={}), setting sentinel nextExecutionTime={}",
                    selected_id,
                    net_result,
                    SENTINEL_FUTURE_TIMESTAMP);
            }
        }

        // Re-acquire lock to update in-memory state only.
        // Persistence is posted to config persist thread (no SettingsManager
        // call under mutex_).
        bool    should_persist = false;
        int64_t persist_id = 0;
        int64_t persist_time = 0;
        {
            const std::scoped_lock lock(mutex_);

            // Update the instance record with refreshed nextExecutionTime
            if (selected_inst && has_refreshed_time)
            {
                selected_inst->next_execution_time = refreshed_time;
                should_persist = true;
                persist_id = selected_id;
                persist_time = refreshed_time;
            }

            current_task_ = nullptr;
            cv_.notify_all();

            if (state_.load() == SchedulerState::Running)
            {
                now_unix = SystemClock::to_time_t(SystemClock::now());
                next_delay = ComputeNextDelay(task_instances_, now_unix);
            }
        }

        DAS_CORE_LOG_DEBUG("SchedulerService::OnTick: tick complete");
        if (state_.load() == SchedulerState::Running)
        {
            StartTickTimer(next_delay);
        }

        // Post persistence event to config-side thread (outside mutex_).
        // Failures are logged by the persist thread and do not affect
        // runtime state.
        if (should_persist)
        {
            PostPersistEvent(persist_id, persist_time);
        }

        // Notify WebSocket clients of state change (server-triggered only)
        if (state_notify_)
        {
            auto state_json = Get();

            auto event = Das::Utils::MakeYyjsonObject();
            auto obj = *event.as_object();
            obj[std::string_view("code")] = static_cast<int64_t>(DAS_S_OK);
            obj[std::string_view("msg")] = std::string_view("");
            obj[std::string_view("data")] = state_json;

            auto state_serialized =
                Das::Utils::SerializeYyjsonValue(event, false);
            if (state_serialized)
            {
                state_notify_(state_serialized->c_str());
            }
        }
    }

    // ----------------------------------------------------------------
    // Config-side persistence thread
    // ----------------------------------------------------------------

    void SchedulerService::SetStateNotifyCallback(
        SchedulerNotifyFunc func,
        void*               user_data)
    {
        state_notify_ = {func, user_data};
    }

    void SchedulerService::PostPersistEvent(int64_t task_id, int64_t next_time)
    {
        {
            const std::scoped_lock lock(config_persist_mutex_);
            config_persist_queue_.push({task_id, next_time});
        }
        config_persist_cv_.notify_one();
    }

    void SchedulerService::ConfigPersistThreadLoop()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(config_persist_mutex_);
            config_persist_cv_.wait(
                lock,
                [this]
                {
                    return !config_persist_queue_.empty()
                           || config_persist_shutdown_.load();
                });

            // Drain all pending events before checking shutdown
            while (!config_persist_queue_.empty())
            {
                auto event = config_persist_queue_.front();
                config_persist_queue_.pop();
                lock.unlock();

                // Execute SettingsManager I/O without holding any
                // SchedulerService::mutex_
                auto& settings = plugin_manager_.GetSettingsManager();
                auto  instance_json =
                    settings.GetTaskInstanceJson("0", event.task_id);
                if (instance_json.is_null() || !instance_json.is_object())
                {
                    instance_json = Das::Utils::MakeYyjsonObject();
                }
                {
                    auto inst_obj = instance_json.as_object();
                    inst_obj->operator[](std::string_view(
                        "nextExecutionTime")) = event.next_execution_time;
                }
                auto result = settings.UpdateTaskInstanceJson(
                    "0",
                    event.task_id,
                    instance_json);

                if (DAS::IsFailed(result))
                {
                    DAS_CORE_LOG_ERROR(
                        "SchedulerService::ConfigPersistThread: failed to "
                        "persist nextExecutionTime for task {}, result={}",
                        event.task_id,
                        result);
                }

                lock.lock();
            }

            if (config_persist_shutdown_.load())
            {
                return;
            }
        }
    }

    void SchedulerService::ShutdownConfigPersistQueue()
    {
        config_persist_shutdown_.store(true);
        config_persist_cv_.notify_all();
        if (config_persist_thread_.joinable())
        {
            config_persist_thread_.join();
        }
    }

} // namespace Das::Core::TaskScheduler

#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>

#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/AuthoringDocContract.h>
#include <das/Core/GraphRuntime/CompiledPlanFingerprint.h>
#include <das/Core/GraphRuntime/FormSequenceProjector.h>
#include <das/Core/GraphRuntime/GraphAuthoring.h>
#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/DasJsonCore.h>
#include <new>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// --- GraphRuntimeImpl implementation ---

GraphRuntimeImpl::GraphRuntimeImpl(
    Das::PluginInterface::IDasTaskComponentHost* p_host)
    : host_(p_host)
{
}

DasResult GraphRuntimeImpl::GetErrorMessage(
    IDasReadOnlyString** pp_out_error_message)
{
    if (!pp_out_error_message)
    {
        return DAS_E_INVALID_POINTER;
    }

    return CreateIDasReadOnlyStringFromUtf8(
        last_error_.c_str(),
        pp_out_error_message);
}

DasResult GraphRuntimeImpl::Execute(
    IDasReadOnlyString*                  p_compiled_artifact_json,
    Das::PluginInterface::IDasStopToken* p_stop_token,
    Das::ExportInterface::IDasJson**     pp_out_result_json)
{
    if (!pp_out_result_json)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_result_json = nullptr;

    // Parse compiled graph plan from JSON string.
    Dto::CompiledGraphPlanDto plan;
    if (!p_compiled_artifact_json)
    {
        // 空指针不是「空图」：静默跑空计划会把调用方的装配缺漏吞成成
        // 功，必须显性拒绝。
        last_error_ = "Execute: compiled artifact is null";
        DAS_CORE_LOG_ERROR("{}", last_error_);
        return DAS_E_INVALID_ARGUMENT;
    }
    {
        const char*     utf8 = nullptr;
        const DasResult hr = p_compiled_artifact_json->GetUtf8(&utf8);
        if (DAS::IsFailed(hr) || !utf8)
        {
            last_error_ = "Execute: failed to read compiled artifact JSON";
            DAS_CORE_LOG_ERROR("{}", last_error_);
            return DAS_E_INVALID_POINTER;
        }

        std::string json_str(utf8);
        if (json_str.empty())
        {
            last_error_ = "Execute: compiled artifact is empty";
            DAS_CORE_LOG_ERROR("{}", last_error_);
            return DAS_E_INVALID_ARGUMENT;
        }
        {
            try
            {
                // The wire artifact is a {"fingerprint","plan"} shell. Verify
                // the plan payload against the shell fingerprint (re-serialize
                // the subtree with the same standard serializer and compare)
                // before executing: a mismatch means the artifact was modified
                // or corrupted after compilation.
                auto shell = yyjson::read(json_str);
                auto shell_obj = shell.as_object();
                if (!shell_obj
                    || !shell_obj->contains(std::string_view("fingerprint"))
                    || !shell_obj->contains(std::string_view("plan")))
                {
                    throw yyjson::bad_cast{
                        "compiled artifact is not a {fingerprint,plan} shell"};
                }
                auto fingerprint =
                    (*shell_obj)[std::string_view("fingerprint")].as_string();
                if (!fingerprint || fingerprint->empty())
                {
                    throw yyjson::bad_cast{
                        "compiled artifact shell carries no fingerprint"};
                }

                const auto& plan_subtree =
                    (*shell_obj)[std::string_view("plan")];
                const std::string computed_fingerprint =
                    ComputeCompiledPlanFingerprint(plan_subtree);
                if (computed_fingerprint.empty()
                    || computed_fingerprint != *fingerprint)
                {
                    last_error_ = "Execute: compiled artifact fingerprint "
                                  "mismatch — the plan payload was modified "
                                  "or corrupted";
                    DAS_CORE_LOG_ERROR("{}", last_error_);
                    return DAS_E_FAIL;
                }

                plan = yyjson::cast<Dto::CompiledGraphPlanDto>(plan_subtree);
            }
            catch (const std::exception& exception)
            {
                last_error_ =
                    std::string{"Execute: invalid compiled artifact JSON: "}
                    + exception.what();
                DAS_CORE_LOG_ERROR("{}", last_error_);
                return DAS_E_INVALID_JSON;
            }
        }
    }

    // Execute via engine. The artifact is self-contained — its integrity gate
    // is the shell fingerprint verified above. There is no live authoring
    // document at execution time to compare freshness against, so the
    // engine's staleness check is explicitly neutralized by self-comparison.
    const DasResult result = engine_.RunWithHost(
        plan,
        plan.source_fingerprint,
        p_stop_token,
        host_.Get());

    if (DAS::IsFailed(result))
    {
        last_error_ = engine_.GetLastErrorMessage();
        if (last_error_.empty())
        {
            last_error_ = "Graph execution failed";
        }
        DAS_CORE_LOG_ERROR("{}", last_error_);
        return result;
    }

    // 成功结果携带 graphOutputs（引擎侧按编译产物边界清单从帧导出的悬空
    // 输出口值映射；空清单/无值时为空对象——消费方按缺键处理即可）。
    yyjson::value result_payload(Das::Utils::MakeYyjsonObject());
    (*result_payload.as_object())[std::string_view("graphOutputs")] =
        Das::Utils::CloneYyjsonValue(engine_.GetLastGraphOutputs());
    auto result_serialized = Das::Utils::SerializeYyjsonValue(result_payload);
    if (!result_serialized.has_value())
    {
        return CreateEmptyDasJson(pp_out_result_json);
    }
    return ParseDasJsonFromString(
        result_serialized->c_str(),
        pp_out_result_json);
}

DAS_CORE_GRAPHRUNTIME_NS_END

// ============================================================================
// C ABI factory function
// ============================================================================

DAS_C_API DasResult
CreateGraphRuntime(Das::ExportInterface::IDasGraphRuntime** pp_out_runtime)
{
    if (!pp_out_runtime)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* impl = new Das::Core::GraphRuntime::GraphRuntimeImpl{nullptr};
        impl->AddRef();
        *pp_out_runtime = impl;
        DAS_CORE_LOG_INFO("CreateGraphRuntime: runtime created successfully");
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("CreateGraphRuntime: out of memory");
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_C_API DasResult CreateGraphRuntimeWithHost(
    Das::PluginInterface::IDasTaskComponentHost* p_host,
    Das::ExportInterface::IDasGraphRuntime**     pp_out_runtime)
{
    if (!pp_out_runtime)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (!p_host)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* impl = new Das::Core::GraphRuntime::GraphRuntimeImpl{p_host};
        impl->AddRef();
        *pp_out_runtime = impl;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ============================================================================
// Stateful authoring session C ABI (DAS-77)
// ============================================================================
//
// The authoritative store (GraphDocumentDto) lives in Core behind an opaque
// handle. Plugins hold the handle and call these per GetDocument / ApplyChange
// / Compile. The public projection is the schema-contract authoring doc
// (AuthoringDocContract); the runtime GraphDocumentDto never crosses.
// ============================================================================

// The Core-owned authoritative store. Opaque to plugins (forward-declared in
// the header); full definition lives only here, at global scope to match the
// header's forward declaration (the C ABI entry points are also global).
//
// `action_entries` carries the registered IDasTaskComponent catalog
// ({actionId,label} objects) translated by the Provider from
// TaskComponentFactoryManager::EnumerateDefinitions() output; the label is
// the component manifest definition's kind. When the session is created
// without a registry handle, the set is empty and actionId-bearing change
// kinds reject as UnknownActionId — the production wiring (Scheduler)
// populates this set when the session is bound to a PluginManager.
struct GraphAuthoringSessionState
{
    Das::Core::GraphRuntime::Dto::GraphDocumentDto document;
    std::vector<Das::Core::GraphRuntime::Contract::ActionCatalogEntry>
        action_entries;
};

namespace
{
    using FormSeqDto = Das::Core::GraphRuntime::Dto::FormSequenceDto;
    using GraphDocDto = Das::Core::GraphRuntime::Dto::GraphDocumentDto;

    bool ReadUtf8(IDasReadOnlyString* p_str, std::string& out)
    {
        if (p_str == nullptr)
        {
            out.clear();
            return true;
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(p_str->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return false;
        }
        out = utf8;
        return true;
    }

    DasResult WrapValue(
        const yyjson::value&             value,
        Das::ExportInterface::IDasJson** out)
    {
        auto serialized = Das::Utils::SerializeYyjsonValue(value);
        if (!serialized)
        {
            return DAS_E_FAIL;
        }
        return ParseDasJsonFromString(serialized->c_str(), out);
    }

    // -------------------------------------------------------------------
    // Context seeding
    // -------------------------------------------------------------------

    template <typename Obj>
    bool ReadStringField(const Obj& obj, std::string_view key, std::string& out)
    {
        if (!obj.contains(key))
        {
            return false;
        }
        if (auto v = obj[key].as_string())
        {
            out = std::string(*v);
            return true;
        }
        return false;
    }

    /// Read a GraphDocumentDto field-by-field from a JSON object (robust manual
    /// parse at the boundary; aggregate cast would also work but this avoids
    /// throwing on partial input).
    template <typename Obj>
    bool ParseGraphDoc(const Obj& obj, GraphDocDto& out)
    {
        out = GraphDocDto{};
        // The metadata keys are optional for raw graph docs: an unguarded
        // operator[] on a missing key throws out_of_range, so guard every
        // read with contains first.
        if (obj.contains(std::string_view("documentId")))
        {
            if (auto v = obj[std::string_view("documentId")].as_string())
            {
                out.document_id = std::string(*v);
            }
        }
        if (obj.contains(std::string_view("version")))
        {
            if (auto v = obj[std::string_view("version")].as_int())
            {
                out.version = static_cast<int32_t>(*v);
            }
        }
        if (obj.contains(std::string_view("fingerprint")))
        {
            if (auto v = obj[std::string_view("fingerprint")].as_string())
            {
                out.fingerprint = std::string(*v);
            }
        }
        if (obj.contains(std::string_view("nodes")))
        {
            auto arr = obj[std::string_view("nodes")].as_array();
            if (arr)
            {
                for (const auto& nv : *arr)
                {
                    try
                    {
                        // yyjson::cast routes through the hand-written
                        // GraphNodeDto caster (missing-key tolerant);
                        // Detail::CastDtoObject would bypass it and hit the
                        // strict aggregate, dropping pre-label nodes.
                        out.nodes.push_back(
                            yyjson::cast<
                                Das::Core::GraphRuntime::Dto::GraphNodeDto>(
                                nv));
                    }
                    catch (...)
                    {
                        // skip malformed node
                    }
                }
            }
        }
        if (obj.contains(std::string_view("edges")))
        {
            auto arr = obj[std::string_view("edges")].as_array();
            if (arr)
            {
                for (const auto& ev : *arr)
                {
                    try
                    {
                        out.edges.push_back(
                            yyjson::cast<
                                Das::Core::GraphRuntime::Dto::GraphEdgeDto>(
                                ev));
                    }
                    catch (...)
                    {
                    }
                }
            }
        }
        if (obj.contains(std::string_view("tags")))
        {
            auto arr = obj[std::string_view("tags")].as_array();
            if (arr)
            {
                for (const auto& tv : *arr)
                {
                    if (auto s = tv.as_string())
                    {
                        out.tags.emplace_back(std::string(*s));
                    }
                }
            }
        }
        // The viewData partition is preserved verbatim (pass-through, no
        // interpretation) so layout coordinates survive the
        // acceptedProperties -> re-seed round-trip.
        if (obj.contains(std::string_view("viewData")))
        {
            out.view_data =
                Das::Utils::CloneYyjsonValue(obj[std::string_view("viewData")]);
        }
        return true;
    }

    // Reverse-map a contract authoring doc (the shape SerializeDocument emits,
    // and the shape the scheduler persists in `properties`) back onto the
    // authoritative GraphDocumentDto store. The formSequence branch accepts
    // both the current contract shape (formSequence.items[]) and the legacy
    // pre-cutover shape (formSequence.sequence[]); graph seeds accept only the
    // full GraphDocumentDto shape (SerializeSource / acceptedProperties —
    // edges carry their declared edgeType), via the raw {nodes} fallback.
    void SeedFromDocValue(
        GraphAuthoringSessionState& s,
        const yyjson::value&        doc)
    {
        auto obj = doc.as_object();
        if (!obj)
        {
            return;
        }

        // Resolve the kind. The contract doc carries {"kind": "formSequence"};
        // a raw formSequence carries "items"; a raw graph doc "nodes".
        std::string kind;
        ReadStringField(*obj, std::string_view("kind"), kind);

        const bool has_form_seq =
            obj->contains(std::string_view("formSequence"));

        if (kind == "formSequence" || has_form_seq)
        {
            // Contract formSequence doc. Two shapes are accepted: the
            // current wire's formSequence.items[] and the legacy
            // pre-cutover formSequence.sequence[] — items first, sequence
            // as the fallback, empty document when neither key exists.
            FormSeqDto seq;
            ReadStringField(
                *obj,
                std::string_view("sourceFingerprint"),
                seq.fingerprint);
            yyjson::value seq_src = doc;
            if (has_form_seq)
            {
                seq_src = (*obj)[std::string_view("formSequence")];
            }
            if (auto seq_obj = seq_src.as_object())
            {
                const std::string_view item_list_key =
                    seq_obj->contains(std::string_view("items"))
                        ? std::string_view("items")
                        : std::string_view("sequence");
                if (seq_obj->contains(item_list_key))
                {
                    if (auto arr = (*seq_obj)[item_list_key].as_array())
                    {
                        for (const auto& iv : *arr)
                        {
                            auto io = iv.as_object();
                            if (!io)
                            {
                                continue;
                            }
                            Das::Core::GraphRuntime::Dto::FormSequenceItemDto
                                item;
                            if (auto v =
                                    (*io)[std::string_view("id")].as_string())
                            {
                                item.item_id = std::string(*v);
                            }
                            std::string guid;
                            // DAS-81: componentGuid (renamed from type).
                            // Accept both for backward compat with persisted
                            // contract docs.
                            if (!ReadStringField(
                                    *io,
                                    std::string_view("componentGuid"),
                                    guid))
                            {
                                ReadStringField(
                                    *io,
                                    std::string_view("type"),
                                    guid);
                            }
                            if (!guid.empty())
                            {
                                item.target.target_kind = "componentRef";
                                item.target.component_ref = Das::Core::
                                    GraphRuntime::Dto::ComponentRefDto{
                                        "componentRef",
                                        guid,
                                        {}};
                            }
                            if (io->contains(std::string_view("settings")))
                            {
                                item.settings = Das::Utils::CloneYyjsonValue(
                                    (*io)[std::string_view("settings")]);
                            }
                            seq.items.push_back(std::move(item));
                        }
                    }
                }
            }
            s.document =
                Das::Core::GraphRuntime::FormSequenceProjector::Project(seq);
            s.document.tags.emplace_back(
                Das::Core::GraphRuntime::Contract::LINEAR_TAG);
            return;
        }

        // Raw formSequence ({items}) / raw graph ({nodes}) fallback.
        if (obj->contains(std::string_view("items")))
        {
            FormSeqDto seq;
            ReadStringField(
                *obj,
                std::string_view("documentId"),
                seq.document_id);
            if (obj->contains(std::string_view("version")))
            {
                if (auto v = (*obj)[std::string_view("version")].as_int())
                {
                    seq.version = static_cast<int32_t>(*v);
                }
            }
            ReadStringField(
                *obj,
                std::string_view("fingerprint"),
                seq.fingerprint);
            if (auto items = (*obj)[std::string_view("items")].as_array())
            {
                for (const auto& iv : *items)
                {
                    try
                    {
                        seq.items.push_back(
                            Das::Core::GraphRuntime::Dto::Detail::CastDtoObject<
                                Das::Core::GraphRuntime::Dto::
                                    FormSequenceItemDto>(iv));
                    }
                    catch (...)
                    {
                    }
                }
            }
            s.document =
                Das::Core::GraphRuntime::FormSequenceProjector::Project(seq);
            s.document.tags.emplace_back(
                Das::Core::GraphRuntime::Contract::LINEAR_TAG);
        }
        else if (obj->contains(std::string_view("nodes")))
        {
            ParseGraphDoc(*obj, s.document);
        }
    }

    void SeedFromContext(GraphAuthoringSessionState& s, std::string_view json)
    {
        if (json.empty())
        {
            return;
        }
        yyjson::value root;
        try
        {
            root = yyjson::read(std::string(json));
        }
        catch (...)
        {
            return;
        }
        auto obj = root.as_object();
        if (!obj)
        {
            return;
        }

        // Real scheduler context shape: {taskId, properties, revision,
        // authoring}. The live store round-trips through `properties` as a
        // contract authoring doc (what SerializeDocument emits and ApplyChange
        // returns as acceptedProperties). Prefer `properties`; fall back to a
        // raw/contract doc at top level for direct callers and tests.
        if (obj->contains(std::string_view("properties")))
        {
            SeedFromDocValue(s, (*obj)[std::string_view("properties")]);
            return;
        }
        if (obj->contains(std::string_view("kind"))
            || obj->contains(std::string_view("items"))
            || obj->contains(std::string_view("nodes")))
        {
            SeedFromDocValue(s, root);
        }
        // else: empty (graph-mode) store.
    }

    // -------------------------------------------------------------------
    // Contract change parsing
    // -------------------------------------------------------------------

    template <typename Obj>
    std::optional<Das::Core::GraphRuntime::Contract::GraphConnection>
    ParseGraphConnection(const Obj& obj)
    {
        Das::Core::GraphRuntime::Contract::GraphConnection c;
        if (!ReadStringField(
                obj,
                std::string_view("fromNodeId"),
                c.from_node_id))
        {
            return std::nullopt;
        }
        ReadStringField(obj, std::string_view("fromPortId"), c.from_port_id);
        if (!ReadStringField(obj, std::string_view("toNodeId"), c.to_node_id))
        {
            return std::nullopt;
        }
        ReadStringField(obj, std::string_view("toPortId"), c.to_port_id);
        return c;
    }

    // Strict Phase 84 AuthoringChange parser. The {baseRevision, kind, payload}
    // envelope is the only accepted shape; legacy flat {op, ...} is rejected
    // upstream as a malformed request. Per-kind payload fields populate the
    // optional slots of AuthoringChange; the dispatch reads them by kind.
    bool ParseAuthoringChange(
        const yyjson::value&                                json,
        Das::Core::GraphRuntime::Contract::AuthoringChange& out,
        int64_t&                                            out_base_revision)
    {
        out = Das::Core::GraphRuntime::Contract::AuthoringChange{};
        out_base_revision = 0;
        auto obj = json.as_object();
        if (!obj)
        {
            return false;
        }
        // baseRevision is required by the strict envelope but only used by the
        // caller for revision-invariant result shaping; this parser is lenient
        // (defaults to 0 when missing) so that test callers and the scheduler
        // both behave predictably.
        if (obj->contains(std::string_view("baseRevision")))
        {
            if (auto v = (*obj)[std::string_view("baseRevision")].as_int())
            {
                out_base_revision = static_cast<int64_t>(*v);
            }
        }
        if (!ReadStringField(*obj, std::string_view("kind"), out.kind))
        {
            return false;
        }
        if (!obj->contains(std::string_view("payload")))
        {
            return false;
        }
        auto payload = (*obj)[std::string_view("payload")].as_object();
        if (!payload)
        {
            return false;
        }

        // setValue payload: {target, fieldName, value}.
        if (out.kind == "setValue")
        {
            if (payload->contains(std::string_view("target")))
            {
                auto target_o =
                    (*payload)[std::string_view("target")].as_object();
                if (target_o)
                {
                    Das::Core::GraphRuntime::Contract::SetValueTarget t;
                    ReadStringField(
                        *target_o,
                        std::string_view("kind"),
                        t.kind);
                    std::string id;
                    if (ReadStringField(*target_o, std::string_view("id"), id)
                        && !id.empty())
                    {
                        t.id = std::move(id);
                    }
                    out.target = std::move(t);
                }
            }
            std::string field_name;
            if (ReadStringField(
                    *payload,
                    std::string_view("fieldName"),
                    field_name)
                && !field_name.empty())
            {
                out.field_name = std::move(field_name);
            }
            if (payload->contains(std::string_view("value")))
            {
                out.value = Das::Utils::CloneYyjsonValue(
                    (*payload)[std::string_view("value")]);
            }
            return true;
        }
        // invokeCommand payload: {commandId}.
        if (out.kind == "invokeCommand")
        {
            std::string command_id;
            if (ReadStringField(
                    *payload,
                    std::string_view("commandId"),
                    command_id)
                && !command_id.empty())
            {
                out.command_id = std::move(command_id);
            }
            return true;
        }
        // convertView payload: {targetViewKind}.
        if (out.kind == "convertView")
        {
            std::string tvk;
            if (ReadStringField(
                    *payload,
                    std::string_view("targetViewKind"),
                    tvk)
                && !tvk.empty())
            {
                out.target_view_kind = std::move(tvk);
            }
            return true;
        }
        // addGraphNode / addFormSequenceItem payload: {actionId,
        // insertionIndex?}.
        if (out.kind == "addGraphNode" || out.kind == "addFormSequenceItem")
        {
            std::string action_id;
            if (ReadStringField(
                    *payload,
                    std::string_view("actionId"),
                    action_id)
                && !action_id.empty())
            {
                out.action_id = std::move(action_id);
            }
            if (payload->contains(std::string_view("insertionIndex")))
            {
                if (auto v =
                        (*payload)[std::string_view("insertionIndex")].as_int())
                {
                    out.insertion_index = static_cast<int64_t>(*v);
                }
            }
            return true;
        }
        // moveFormSequenceItem payload: {formSequenceItemId, destinationIndex}.
        if (out.kind == "moveFormSequenceItem")
        {
            std::string id;
            if (ReadStringField(
                    *payload,
                    std::string_view("formSequenceItemId"),
                    id)
                && !id.empty())
            {
                out.form_sequence_item_id = std::move(id);
            }
            if (payload->contains(std::string_view("destinationIndex")))
            {
                if (auto v = (*payload)[std::string_view("destinationIndex")]
                                 .as_int())
                {
                    out.destination_index = static_cast<int64_t>(*v);
                }
            }
            return true;
        }
        // moveGraphNode payload: {nodeId, position:{x,y}}. The coordinates
        // ride the document-level viewData partition (not node.settings).
        // as_num accepts both "120" and "120.5" so integer-formatted JSON
        // coordinates deserialize identically.
        if (out.kind == "moveGraphNode")
        {
            std::string id;
            if (ReadStringField(*payload, std::string_view("nodeId"), id)
                && !id.empty())
            {
                out.item_or_node_id = std::move(id);
            }
            if (payload->contains(std::string_view("position")))
            {
                if (auto position =
                        (*payload)[std::string_view("position")].as_object())
                {
                    if (position->contains(std::string_view("x")))
                    {
                        if (auto x_value =
                                (*position)[std::string_view("x")].as_num())
                        {
                            out.position_x = *x_value;
                        }
                    }
                    if (position->contains(std::string_view("y")))
                    {
                        if (auto y_value =
                                (*position)[std::string_view("y")].as_num())
                        {
                            out.position_y = *y_value;
                        }
                    }
                }
            }
            return true;
        }
        // removeGraphNodes payload: {nodeIds[], connections?[]}. Batch node
        // removal — ApplyGraphChange validates all ids and resolves all
        // connections before mutating (atomic validate-all-then-mutate).
        if (out.kind == "removeGraphNodes")
        {
            if (payload->contains(std::string_view("nodeIds")))
            {
                if (auto ids =
                        (*payload)[std::string_view("nodeIds")].as_array())
                {
                    std::vector<std::string> node_ids;
                    for (const auto& item : *ids)
                    {
                        if (auto id = item.as_string())
                        {
                            node_ids.emplace_back(*id);
                        }
                    }
                    out.removed_node_ids = std::move(node_ids);
                }
            }
            if (payload->contains(std::string_view("connections")))
            {
                if (auto connections =
                        (*payload)[std::string_view("connections")].as_array())
                {
                    std::vector<
                        Das::Core::GraphRuntime::Contract::GraphConnection>
                        parsed_connections;
                    for (const auto& connection : *connections)
                    {
                        auto connection_obj = connection.as_object();
                        if (!connection_obj)
                        {
                            continue;
                        }
                        if (auto parsed = ParseGraphConnection(*connection_obj))
                        {
                            parsed_connections.push_back(std::move(*parsed));
                        }
                    }
                    out.removed_connections = std::move(parsed_connections);
                }
            }
            return true;
        }
        // moveGraphNodes payload: {moves[]} — each entry is
        // {nodeId, position:{x,y}}. as_num accepts both "120" and "120.5"
        // so integer-formatted JSON coordinates deserialize identically.
        if (out.kind == "moveGraphNodes")
        {
            if (payload->contains(std::string_view("moves")))
            {
                if (auto moves =
                        (*payload)[std::string_view("moves")].as_array())
                {
                    std::vector<
                        Das::Core::GraphRuntime::Contract::GraphNodeMove>
                        node_moves;
                    for (const auto& item : *moves)
                    {
                        auto move_obj = item.as_object();
                        if (!move_obj)
                        {
                            continue;
                        }
                        Das::Core::GraphRuntime::Contract::GraphNodeMove
                                    node_move;
                        std::string id;
                        if (ReadStringField(
                                *move_obj,
                                std::string_view("nodeId"),
                                id)
                            && !id.empty())
                        {
                            node_move.node_id = std::move(id);
                        }
                        if (move_obj->contains(std::string_view("position")))
                        {
                            if (auto position =
                                    (*move_obj)[std::string_view("position")]
                                        .as_object())
                            {
                                if (position->contains(std::string_view("x")))
                                {
                                    if (auto x_value =
                                            (*position)[std::string_view("x")]
                                                .as_num())
                                    {
                                        node_move.position_x = *x_value;
                                    }
                                }
                                if (position->contains(std::string_view("y")))
                                {
                                    if (auto y_value =
                                            (*position)[std::string_view("y")]
                                                .as_num())
                                    {
                                        node_move.position_y = *y_value;
                                    }
                                }
                            }
                        }
                        node_moves.push_back(std::move(node_move));
                    }
                    out.node_moves = std::move(node_moves);
                }
            }
            return true;
        }
        // renameGraphNode payload: {nodeId, label}. The label is pure
        // presentation data written onto the store node; addressing stays on
        // the node id (same slot convention as moveGraphNode).
        if (out.kind == "renameGraphNode")
        {
            std::string id;
            if (ReadStringField(*payload, std::string_view("nodeId"), id)
                && !id.empty())
            {
                out.item_or_node_id = std::move(id);
            }
            std::string label;
            if (ReadStringField(*payload, std::string_view("label"), label)
                && !label.empty())
            {
                out.rename_label = std::move(label);
            }
            return true;
        }
        // removeFormSequenceItem / removeGraphNode payload:
        // {formSequenceItemId} or {nodeId}. Both are surfaced via
        // item_or_node_id.
        if (out.kind == "removeFormSequenceItem"
            || out.kind == "removeGraphNode")
        {
            std::string id;
            const auto  key = out.kind == "removeFormSequenceItem"
                                  ? std::string_view("formSequenceItemId")
                                  : std::string_view("nodeId");
            if (ReadStringField(*payload, key, id) && !id.empty())
            {
                out.item_or_node_id = std::move(id);
            }
            return true;
        }
        // connectGraphPorts / disconnectGraphPorts payload: connection fields.
        if (out.kind == "connectGraphPorts"
            || out.kind == "disconnectGraphPorts")
        {
            out.connection = ParseGraphConnection(*payload);
            return true;
        }
        // Unknown kinds are surfaced as parse failure; the dispatcher emits an
        // InvalidOp diagnostic. The strict upstream validator would have
        // rejected them earlier.
        return true;
    }

    /// Optional ActionCatalog projection from the session's stored registry
    /// snapshot. Both the Get and the Apply document serialisation paths must
    /// pass it so the node port projection (static registry ports merged by
    /// componentGuid) stays identical across GetDocument and every Apply
    /// round-trip.
    std::optional<Das::Core::GraphRuntime::Contract::ActionCatalog>
    MakeCatalogFromState(const GraphAuthoringSessionState& state)
    {
        if (state.action_entries.empty())
        {
            return std::nullopt;
        }
        Das::Core::GraphRuntime::Contract::ActionCatalog catalog;
        catalog.entries = state.action_entries;
        return catalog;
    }

    /// Build the apply-result JSON. Per D-84-08/D-84-10, the result envelope
    /// is {acceptedProperties, document, diagnostics[]}; per the Phase 84
    /// validator (plan 01), `document.revision` must equal `baseRevision + 1`
    /// on success. The post-apply store version is reflected directly inside
    /// the emitted document; `base_revision` is retained for documentation.
    yyjson::value BuildChangeResultJson(
        const Das::Core::GraphRuntime::Contract::AuthoringChangeResult& r,
        int32_t            base_revision,
        const GraphDocDto& document,
        const std::optional<Das::Core::GraphRuntime::Contract::ActionCatalog>&
            action_catalog)
    {
        auto root = Das::Utils::MakeYyjsonObject();
        auto obj = root.as_object();

        // diagnostics[] — strict Phase 84 shape (D-84-11).
        auto diag_arr = Das::Utils::MakeYyjsonArray();

        if (r.Ok())
        {
            // D-41: acceptedProperties = source (persisted GraphDocumentDto);
            // document = regenerated UI view. Distinct blobs — no double-stuff.
            (*obj)[std::string_view("acceptedProperties")] =
                Das::Core::GraphRuntime::Contract::SerializeSource(document);
            (*obj)[std::string_view("document")] =
                Das::Core::GraphRuntime::Contract::SerializeDocument(
                    document,
                    action_catalog);
        }
        else
        {
            // Strict Phase 84 ApplyResult envelope is {acceptedProperties,
            // document, diagnostics[]} — no top-level errorKind/message. The
            // error is surfaced as a diagnostic with severity:error so the
            // Scheduler can pass it through to the client. An optional integer
            // DasResult code is attached when the caller mapped the error kind
            // to a stable DasResult value; the strict validator allows code on
            // Diagnostic. Here we keep code absent and rely on the message
            // alone — GraphRuntime-local errors are not DasResult-coded.
            auto diag_obj = Das::Utils::MakeYyjsonObject();
            auto d_o = diag_obj.as_object();
            if (d_o)
            {
                (*d_o)[std::string_view("severity")] = std::make_pair(
                    std::string_view("error"),
                    yyjson::copy_string);
                (*d_o)[std::string_view("message")] = std::make_pair(
                    std::string_view(r.message),
                    yyjson::copy_string);
            }
            auto diag_arr_o = diag_arr.as_array();
            if (diag_arr_o)
            {
                diag_arr_o->emplace_back(std::move(diag_obj));
            }
            // On failure, the document is the unchanged pre-apply state — emit
            // it so the client sees a fresh authoritative snapshot. The
            // revision reflects the unchanged store.
            (*obj)[std::string_view("document")] =
                Das::Core::GraphRuntime::Contract::SerializeDocument(
                    document,
                    action_catalog);
            // acceptedProperties remains the persisted source so the client
            // sees the unchanged authoritative state.
            (*obj)[std::string_view("acceptedProperties")] =
                Das::Core::GraphRuntime::Contract::SerializeSource(document);
            // baseRevision-based invariant: on failure, document.revision must
            // equal base (no change). The validator only rejects on Apply
            // success paths where document.revision != base+1; failure paths
            // surface diagnostics instead.
            std::ignore = base_revision;
        }
        (*obj)[std::string_view("diagnostics")] = std::move(diag_arr);

        // The allocated id (D-84-38) is NOT emitted as a separate envelope
        // field — the strict Phase 84 Apply result validator
        // (ValidateAuthoringApplyResult) rejects unknown fields. Callers read
        // the freshly-allocated id from the returned document's nodes[]/items[]
        // instead. Failure-path diagnostics surface errorKind/message only via
        // the diagnostics[] array (no top-level errorKind field on the
        // envelope).
        std::ignore = r.allocated_id;
        return root;
    }

    // Catalog 端口对象的锁形规则（单一来源）：必填 portId/portType/
    // direction，可选 displayLabel/loopCarried；键集外任何键、空 id/type、
    // direction 不在 {input,output}、非字符串 displayLabel、非布尔
    // loopCarried 均为形状违规。会话目录注入入口与端口面构建段共用本
    // 判定——两处曾各自手抄键清单，只改一份时带新键的目录被另一份
    // 整体拒收（添加节点面板全空）；改键集/值域只改这里。
    bool CatalogPortShapeValid(const yyjson::value& port_value)
    {
        auto port_obj = port_value.as_object();
        if (!port_obj.has_value()
            || !port_obj->contains(std::string_view("portId"))
            || !port_obj->contains(std::string_view("portType"))
            || !port_obj->contains(std::string_view("direction")))
        {
            return false;
        }
        for (const auto& [key, value] : *port_obj)
        {
            (void)value;
            if (key != std::string_view("portId")
                && key != std::string_view("portType")
                && key != std::string_view("direction")
                && key != std::string_view("displayLabel")
                && key != std::string_view("loopCarried"))
            {
                return false;
            }
        }
        const auto& port = *port_obj;
        const auto  id = port[std::string_view("portId")].as_string();
        if (!id.has_value() || id->empty())
        {
            return false;
        }
        const auto type = port[std::string_view("portType")].as_string();
        if (!type.has_value() || type->empty())
        {
            return false;
        }
        const auto direction = port[std::string_view("direction")].as_string();
        if (!direction.has_value()
            || (*direction != std::string_view("input")
                && *direction != std::string_view("output")))
        {
            return false;
        }
        if (port.contains(std::string_view("displayLabel"))
            && !port[std::string_view("displayLabel")].as_string().has_value())
        {
            return false;
        }
        if (port.contains(std::string_view("loopCarried"))
            && !port[std::string_view("loopCarried")].as_bool().has_value())
        {
            return false;
        }
        return true;
    }

    // 边界推导 C ABI 的 catalog 端口面解析：只消费 actionId + ports 通道，
    // 元素形状与 SetActionCatalog 同锁（共享 CatalogPortShapeValid 单一
    // 来源），同 all-or-nothing 拒绝语义——畸形输入整体拒绝（nullopt），
    // 不落半截结果。
    std::optional<std::unordered_map<
        std::string,
        Das::Core::GraphRuntime::GraphCompiler::ComponentPortSurface>>
    ParseCatalogPortSurfaces(const yyjson::value& root)
    {
        auto arr = root.as_array();
        if (!arr.has_value())
        {
            return std::nullopt;
        }
        std::unordered_map<
            std::string,
            Das::Core::GraphRuntime::GraphCompiler::ComponentPortSurface>
            surfaces;
        for (const auto& entry_value : *arr)
        {
            auto entry_obj = entry_value.as_object();
            if (!entry_obj.has_value()
                || !entry_obj->contains(std::string_view("actionId")))
            {
                return std::nullopt;
            }
            auto action_id =
                (*entry_obj)[std::string_view("actionId")].as_string();
            if (!action_id.has_value() || action_id->empty())
            {
                return std::nullopt;
            }
            Das::Core::GraphRuntime::GraphCompiler::ComponentPortSurface
                surface;
            if (entry_obj->contains(std::string_view("ports")))
            {
                auto ports_value =
                    (*entry_obj)[std::string_view("ports")].as_array();
                if (!ports_value.has_value())
                {
                    return std::nullopt;
                }
                for (const auto& port_value : *ports_value)
                {
                    auto port_obj = port_value.as_object();
                    if (!port_obj.has_value()
                        || !CatalogPortShapeValid(port_value))
                    {
                        return std::nullopt;
                    }
                    Das::Core::GraphRuntime::Dto::GraphPortDefinitionDto
                         definition;
                    auto id =
                        (*port_obj)[std::string_view("portId")].as_string();
                    if (!id.has_value() || id->empty())
                    {
                        return std::nullopt;
                    }
                    definition.port_id = *id;
                    auto type =
                        (*port_obj)[std::string_view("portType")].as_string();
                    if (!type.has_value() || type->empty())
                    {
                        return std::nullopt;
                    }
                    definition.port_type = *type;
                    if (port_obj->contains(std::string_view("displayLabel")))
                    {
                        auto display_label =
                            (*port_obj)[std::string_view("displayLabel")]
                                .as_string();
                        if (!display_label.has_value())
                        {
                            return std::nullopt;
                        }
                        definition.display_label = *display_label;
                    }
                    auto direction =
                        (*port_obj)[std::string_view("direction")].as_string();
                    if (!direction.has_value())
                    {
                        return std::nullopt;
                    }
                    if (port_obj->contains(std::string_view("loopCarried")))
                    {
                        auto carried =
                            (*port_obj)[std::string_view("loopCarried")]
                                .as_bool();
                        if (!carried.has_value())
                        {
                            return std::nullopt;
                        }
                        definition.loop_carried = *carried;
                    }
                    if (*direction == std::string_view("input"))
                    {
                        surface.inputs.push_back(std::move(definition));
                    }
                    else if (*direction == std::string_view("output"))
                    {
                        surface.outputs.push_back(std::move(definition));
                    }
                    else
                    {
                        return std::nullopt;
                    }
                }
            }
            surfaces[std::string(*action_id)] = std::move(surface);
        }
        return surfaces;
    }

    // 新建空图预置 begin+end 两节点：仅当 seed 后文档仍空且非
    // formSequence 视图（LINEAR_TAG）——formSequence 上下文与非空图不
    // 预置。内部 DTO 直写绕过 addGraphNode 校验（时序上 catalog 尚未就
    // 位——节点只携带 componentGuid，合法）；节点 id 用 n_0/n_1，与
    // AllocateStableNodeId 在空文档上的产出逐字相同（计数器从
    // existing.size() 起扫），后续 addGraphNode 顺延到 n_2。
    void SeedBoundaryNodesForNewGraph(GraphAuthoringSessionState& s)
    {
        if (!s.document.nodes.empty()
            || Das::Core::GraphRuntime::Contract::IsLinear(s.document))
        {
            return;
        }
        const auto make_boundary_node =
            [](const std::string_view node_id,
               const std::string_view component_guid)
        {
            Das::Core::GraphRuntime::Dto::GraphNodeDto node;
            node.node_id = std::string(node_id);
            node.target.target_kind = "componentRef";
            node.target.component_ref =
                Das::Core::GraphRuntime::Dto::ComponentRefDto{
                    "componentRef",
                    std::string(component_guid),
                    std::string(
                        Das::Core::GraphRuntime::GRAPH_TASK_PLUGIN_GUID)};
            return node;
        };
        s.document.nodes.push_back(make_boundary_node(
            "n_0",
            Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID));
        s.document.nodes.push_back(make_boundary_node(
            "n_1",
            Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID));
    }
} // anonymous namespace

DAS_C_API DasResult CreateGraphAuthoringSession(
    IDasReadOnlyString*          p_context_json,
    GraphAuthoringSessionState** pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out = nullptr;

    try
    {
        auto*       s = new GraphAuthoringSessionState();
        std::string ctx;
        if (ReadUtf8(p_context_json, ctx))
        {
            SeedFromContext(*s, ctx);
        }
        SeedBoundaryNodesForNewGraph(*s);
        *pp_out = s;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_C_API void DestroyGraphAuthoringSession(GraphAuthoringSessionState* p)
{
    delete p;
}

DAS_C_API DasResult GraphAuthoringSessionGetDocument(
    GraphAuthoringSessionState*      p,
    IDasReadOnlyString*              p_request_json,
    Das::ExportInterface::IDasJson** pp_out_document_json)
{
    std::ignore = p_request_json;
    if (p == nullptr || pp_out_document_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_document_json = nullptr;

    try
    {
        // Build the optional ActionCatalog projection from the Provider-fed
        // entries. GraphAuthoringSessionSetActionCatalog stores
        // the {actionId,label} objects as-is (label already carries the
        // manifest kind, with the actionId fallback applied at parse time),
        // so the projection is a direct copy of the stored catalog.
        const auto catalog = MakeCatalogFromState(*p);
        // D-84-08: 严格 Get envelope = {document, diagnostics}。
        // SchedulerService ValidateAuthoringGetResult 要求 top-level
        // diagnostics 数组（即使空）。Core authoring 没有 PiDiagnostic 来源，
        // diagnostics 永远为空数组（满足 validator required 约束）。
        auto document = Das::Core::GraphRuntime::Contract::SerializeDocument(
            p->document,
            catalog);

        yyjson::value envelope(Das::Utils::MakeYyjsonObject());
        auto          env_obj = envelope.as_object();
        (*env_obj)[std::string_view("document")] = std::move(document);
        (*env_obj)[std::string_view("diagnostics")] =
            Das::Utils::MakeYyjsonArray();

        auto str = Das::Utils::SerializeYyjsonValue(envelope);
        if (!str)
        {
            return DAS_E_FAIL;
        }
        return ParseDasJsonFromString(str->c_str(), pp_out_document_json);
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_FAIL;
    }
}

DAS_C_API DasResult GraphAuthoringSessionApplyChange(
    GraphAuthoringSessionState*      p,
    IDasReadOnlyString*              p_change_json,
    Das::ExportInterface::IDasJson** pp_out_result_json)
{
    if (p == nullptr || pp_out_result_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_result_json = nullptr;

    try
    {
        std::string change_utf8;
        if (!ReadUtf8(p_change_json, change_utf8))
        {
            return DAS_E_INVALID_POINTER;
        }

        Das::Core::GraphRuntime::Contract::AuthoringChange change;
        int64_t                                            base_revision = 0;
        if (change_utf8.empty())
        {
            auto r = WrapValue(
                BuildChangeResultJson(
                    {Das::Core::GraphRuntime::Contract::ChangeErrorKind::
                         InvalidOp,
                     "empty change request",
                     {},
                     std::nullopt},
                    p->document.version,
                    p->document,
                    MakeCatalogFromState(*p)),
                pp_out_result_json);
            return r;
        }

        auto doc = yyjson::read(change_utf8);
        if (!ParseAuthoringChange(doc, change, base_revision))
        {
            return WrapValue(
                BuildChangeResultJson(
                    {Das::Core::GraphRuntime::Contract::ChangeErrorKind::
                         InvalidOp,
                     "malformed change request",
                     {},
                     std::nullopt},
                    p->document.version,
                    p->document,
                    MakeCatalogFromState(*p)),
                pp_out_result_json);
        }

        std::vector<std::string> valid_action_ids;
        valid_action_ids.reserve(p->action_entries.size());
        for (const auto& entry : p->action_entries)
        {
            valid_action_ids.push_back(entry.action_id);
        }
        // The declared port surface rides the same session catalog the
        // compile path injects via SetPortSurfaceProvider — connectGraphPorts
        // derives the stored edge_type from the endpoint port categories
        // through it (canonical signal port names resolve with no
        // declaration; undeclared endpoints reject).
        auto r = Das::Core::GraphRuntime::Contract::ApplyAuthoringChange(
            p->document,
            change,
            valid_action_ids,
            MakeCatalogFromState(*p));
        return WrapValue(
            BuildChangeResultJson(
                r,
                static_cast<int32_t>(base_revision),
                p->document,
                MakeCatalogFromState(*p)),
            pp_out_result_json);
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_FAIL;
    }
}

DAS_C_API DasResult GraphAuthoringSessionCompile(
    GraphAuthoringSessionState*      p,
    IDasReadOnlyString*              p_request_json,
    Das::ExportInterface::IDasJson** pp_out_compiled_plan_json)
{
    std::ignore = p_request_json;
    if (p == nullptr || pp_out_compiled_plan_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_compiled_plan_json = nullptr;

    try
    {
        // No factory manager bound at the authoring boundary, so manifest port
        // validation is skipped (acceptable for authoring preview).
        Das::Core::GraphRuntime::GraphCompiler compiler;
        // 端口面注入（与 required 清单同范式）：catalog 声明的静态端口面
        // （direction 由 inputs/outputs 声明携带）喂给编译器，使无 factory
        // manager 的 authoring 编译路径也能做端口校验与边界端口推导——
        // 生产执行输入正是这条编译路径的产物。
        compiler.SetPortSurfaceProvider(
            [&action_entries =
                 p->action_entries](const std::string& component_guid)
                -> std::optional<Das::Core::GraphRuntime::GraphCompiler::
                                     ComponentPortSurface>
            {
                for (const auto& entry : action_entries)
                {
                    if (entry.action_id != component_guid)
                    {
                        continue;
                    }
                    Das::Core::GraphRuntime::GraphCompiler::ComponentPortSurface
                        surface;
                    for (const auto& port : entry.ports)
                    {
                        Das::Core::GraphRuntime::Dto::GraphPortDefinitionDto
                            definition;
                        definition.port_id = port.port_id;
                        definition.display_label = port.display_label;
                        definition.port_type = port.port_type;
                        if (port.direction == std::string_view("input"))
                        {
                            surface.inputs.push_back(std::move(definition));
                        }
                        else
                        {
                            surface.outputs.push_back(std::move(definition));
                        }
                    }
                    return surface;
                }
                return std::nullopt;
            });
        // required 清单由会话 catalog 推导注入（compiler 无 catalog 访问）：
        // action_id 即 GraphTask 域的 componentGuid，config 字段的 required
        // 声明聚合成 component_guid → 字段名集，编译期对空值产出
        // RequiredFieldMissing diagnostic。
        std::unordered_map<std::string, std::unordered_set<std::string>>
            required_fields;
        for (const auto& entry : p->action_entries)
        {
            for (const auto& config_field : entry.config_fields)
            {
                if (config_field.required)
                {
                    required_fields[entry.action_id].insert(config_field.name);
                }
            }
        }
        auto plan = compiler.Compile(p->document, required_fields);

        // Strict authoring compile envelope: the scheduler validates Compile
        // results with OnlyKeys {compile, diagnostics} and extracts the task
        // input from compile.executionInput. The wire artifact traveling
        // inside executionInput is the {"fingerprint","plan"} shell: the
        // whole CompiledGraphPlanDto rides under "plan" and its FNV-1a 64
        // content fingerprint under "fingerprint" (GraphRuntime::Execute
        // re-computes and compares before executing; DasGraphTaskAdapter::Do
        // is a pure pass-through).
        yyjson::value envelope(Das::Utils::MakeYyjsonObject());
        auto          envelope_obj = envelope.as_object();

        yyjson::value compile_section(Das::Utils::MakeYyjsonObject());
        (*compile_section.as_object())[std::string_view("executionInput")] =
            Das::Core::GraphRuntime::BuildCompiledPlanShell(
                yyjson::object(plan));
        (*envelope_obj)[std::string_view("compile")] =
            std::move(compile_section);

        // The validator requires diagnostics to be an array even when empty.
        yyjson::value diagnostics(Das::Utils::MakeYyjsonArray());
        auto          diagnostics_arr = diagnostics.as_array();
        for (const auto& diagnostic : plan.diagnostics)
        {
            diagnostics_arr->emplace_back(
                Das::Utils::CloneYyjsonValue(diagnostic));
        }
        (*envelope_obj)[std::string_view("diagnostics")] =
            std::move(diagnostics);

        return WrapValue(std::move(envelope), pp_out_compiled_plan_json);
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_FAIL;
    }
}

DAS_C_API DasResult
GraphAuthoringSessionUpgradeToGraph(GraphAuthoringSessionState* p)
{
    if (p == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    return Das::Core::GraphRuntime::Contract::UpgradeToGraph(p->document)
               ? DAS_S_OK
               : DAS_E_FAIL;
}

// Set the registered action catalog the session will accept on addGraphNode /
// addFormSequenceItem (D-84-31/32/33) and surface as actionCatalog entries on
// GetDocument. `p_action_catalog_json` is a JSON array of {actionId,label}
// objects (the Provider translates
// TaskComponentFactoryManager::EnumerateDefinitions() output into this form);
// pass null to clear. Shape validation is business-neutral: every entry must
// be an object carrying a non-empty string actionId; label is an optional
// string and falls back to the actionId when absent or empty. Each entry may
// also carry a `ports` array — the registry-declared static port surface of
// the component — whose elements must be exactly
// {portId,portType,direction[,displayLabel]} with direction in
// {"input","output"}. Malformed input rejects the whole catalog and leaves
// the session untouched.
DAS_C_API DasResult GraphAuthoringSessionSetActionCatalog(
    GraphAuthoringSessionState* p,
    IDasReadOnlyString*         p_action_catalog_json)
{
    if (p == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (p_action_catalog_json == nullptr)
    {
        p->action_entries.clear();
        return DAS_S_OK;
    }
    const char* utf8 = nullptr;
    if (DAS::IsFailed(p_action_catalog_json->GetUtf8(&utf8)) || utf8 == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    try
    {
        auto root = yyjson::read(std::string(utf8));
        auto arr = root.as_array();
        if (!arr)
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        std::vector<Das::Core::GraphRuntime::Contract::ActionCatalogEntry>
            entries;
        entries.reserve(arr->size());
        for (const auto& v : *arr)
        {
            auto entry_obj = v.as_object();
            if (!entry_obj
                || !entry_obj->contains(std::string_view("actionId")))
            {
                return DAS_E_INVALID_ARGUMENT;
            }
            auto action_id =
                (*entry_obj)[std::string_view("actionId")].as_string();
            if (!action_id || action_id->empty())
            {
                return DAS_E_INVALID_ARGUMENT;
            }
            Das::Core::GraphRuntime::Contract::ActionCatalogEntry entry;
            entry.action_id = *action_id;
            // Missing/empty label falls back to the actionId; a present
            // non-string label is a shape violation.
            entry.label = *action_id;
            if (entry_obj->contains(std::string_view("label")))
            {
                auto label =
                    (*entry_obj)[std::string_view("label")].as_string();
                if (!label)
                {
                    return DAS_E_INVALID_ARGUMENT;
                }
                if (!label->empty())
                {
                    entry.label = *label;
                }
            }
            if (entry_obj->contains(std::string_view("ports")))
            {
                auto ports_val =
                    (*entry_obj)[std::string_view("ports")].as_array();
                if (!ports_val)
                {
                    return DAS_E_INVALID_ARGUMENT;
                }
                entry.ports.reserve(ports_val->size());
                for (const auto& port_val : *ports_val)
                {
                    auto port_obj = port_val.as_object();
                    // 端口锁形规则单一来源：CatalogPortShapeValid（与端口
                    // 面构建段共享——改键集/值域只改那里）。loopCarried 在
                    // 此只验不存：wire 投影不携带它，端口面通道从目录原文
                    // 重读。
                    if (!port_obj || !CatalogPortShapeValid(port_val))
                    {
                        return DAS_E_INVALID_ARGUMENT;
                    }
                    Das::Core::GraphRuntime::Contract::GraphNodePort port;
                    if (auto id =
                            (*port_obj)[std::string_view("portId")].as_string();
                        id && !id->empty())
                    {
                        port.port_id = *id;
                    }
                    else
                    {
                        return DAS_E_INVALID_ARGUMENT;
                    }
                    if (auto type = (*port_obj)[std::string_view("portType")]
                                        .as_string();
                        type && !type->empty())
                    {
                        port.port_type = *type;
                    }
                    else
                    {
                        return DAS_E_INVALID_ARGUMENT;
                    }
                    auto direction =
                        (*port_obj)[std::string_view("direction")].as_string();
                    if (direction
                        && (*direction == std::string_view("input")
                            || *direction == std::string_view("output")))
                    {
                        port.direction = *direction;
                    }
                    else
                    {
                        return DAS_E_INVALID_ARGUMENT;
                    }
                    if (port_obj->contains(std::string_view("displayLabel")))
                    {
                        auto display_label =
                            (*port_obj)[std::string_view("displayLabel")]
                                .as_string();
                        if (!display_label)
                        {
                            return DAS_E_INVALID_ARGUMENT;
                        }
                        port.display_label = *display_label;
                    }
                    entry.ports.push_back(std::move(port));
                }
            }
            // Optional config channel:
            // {name,type,default?,taskGuid?,label?,required?,options?}
            // elements with the element key set locked to exactly those keys
            // — the same whole-catalog rejection on malformed input as the
            // port channel.
            if (entry_obj->contains(std::string_view("config")))
            {
                auto config_val =
                    (*entry_obj)[std::string_view("config")].as_array();
                if (!config_val)
                {
                    return DAS_E_INVALID_ARGUMENT;
                }
                entry.config_fields.reserve(config_val->size());
                for (const auto& config_item : *config_val)
                {
                    auto config_obj = config_item.as_object();
                    if (!config_obj
                        || !config_obj->contains(std::string_view("name"))
                        || !config_obj->contains(std::string_view("type")))
                    {
                        return DAS_E_INVALID_ARGUMENT;
                    }
                    for (const auto& [key, value] : *config_obj)
                    {
                        (void)value;
                        if (key != std::string_view("name")
                            && key != std::string_view("type")
                            && key != std::string_view("default")
                            && key != std::string_view("taskGuid")
                            && key != std::string_view("label")
                            && key != std::string_view("required")
                            && key != std::string_view("options"))
                        {
                            return DAS_E_INVALID_ARGUMENT;
                        }
                    }
                    auto name =
                        (*config_obj)[std::string_view("name")].as_string();
                    if (!name || name->empty())
                    {
                        return DAS_E_INVALID_ARGUMENT;
                    }
                    auto type =
                        (*config_obj)[std::string_view("type")].as_string();
                    if (!type || type->empty())
                    {
                        return DAS_E_INVALID_ARGUMENT;
                    }
                    Das::Core::GraphRuntime::Contract::ConfigFieldDescriptor
                        field;
                    field.name = *name;
                    field.type = *type;
                    if (config_obj->contains(std::string_view("default")))
                    {
                        const auto& default_value =
                            (*config_obj)[std::string_view("default")];
                        if (!default_value.is_null())
                        {
                            field.default_value =
                                Das::Utils::CloneYyjsonValue(default_value);
                        }
                    }
                    // Optional declared target task type of a
                    // component-domain field: a non-string value is a shape
                    // violation; an empty string reads as "none declared"
                    // (the same engaged-only reading the producer applies).
                    if (config_obj->contains(std::string_view("taskGuid")))
                    {
                        auto task_guid =
                            (*config_obj)[std::string_view("taskGuid")]
                                .as_string();
                        if (!task_guid)
                        {
                            return DAS_E_INVALID_ARGUMENT;
                        }
                        if (!task_guid->empty())
                        {
                            field.task_guid = *task_guid;
                        }
                    }
                    // Optional producer-resolved display label of the field:
                    // a non-string value is a shape violation; an empty or
                    // absent label reads as "none declared" and the fields
                    // projection falls back to the field name (the same
                    // engaged-only reading the producer applies).
                    if (config_obj->contains(std::string_view("label")))
                    {
                        auto label = (*config_obj)[std::string_view("label")]
                                         .as_string();
                        if (!label)
                        {
                            return DAS_E_INVALID_ARGUMENT;
                        }
                        if (!label->empty())
                        {
                            field.label = *label;
                        }
                    }
                    // Optional required declaration: a non-bool value is a
                    // shape violation rejecting the whole catalog; absent or
                    // false reads as not required (the projection always
                    // emits the boolean so consumers see the declaration).
                    if (config_obj->contains(std::string_view("required")))
                    {
                        auto required =
                            (*config_obj)[std::string_view("required")]
                                .as_bool();
                        if (!required)
                        {
                            return DAS_E_INVALID_ARGUMENT;
                        }
                        field.required = *required;
                    }
                    // Optional closed candidate-list declaration: a non-empty
                    // array of strings (the field's value domain, e.g. an
                    // operator vocabulary). A non-array value or any
                    // non-string/empty element is a shape violation rejecting
                    // the whole catalog (fail-closed); an empty array reads
                    // as not declared (engaged-only, the producer never
                    // emits one).
                    if (config_obj->contains(std::string_view("options")))
                    {
                        auto options =
                            (*config_obj)[std::string_view("options")]
                                .as_array();
                        if (!options)
                        {
                            return DAS_E_INVALID_ARGUMENT;
                        }
                        field.options.reserve(options->size());
                        for (const auto& option : *options)
                        {
                            auto option_text = option.as_string();
                            if (!option_text || option_text->empty())
                            {
                                return DAS_E_INVALID_ARGUMENT;
                            }
                            field.options.emplace_back(*option_text);
                        }
                    }
                    entry.config_fields.push_back(std::move(field));
                }
            }
            entries.push_back(std::move(entry));
        }
        p->action_entries = std::move(entries);
        return DAS_S_OK;
    }
    catch (...)
    {
        return DAS_E_FAIL;
    }
}

DAS_C_API DasResult GraphDeriveBoundaryPorts(
    IDasReadOnlyString*  p_document_json,
    IDasReadOnlyString*  p_catalog_json,
    IDasReadOnlyString** pp_out_boundary_json)
{
    if (pp_out_boundary_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_boundary_json = nullptr;

    std::string document_text;
    if (!ReadUtf8(p_document_json, document_text) || document_text.empty())
    {
        return DAS_E_INVALID_POINTER;
    }
    std::string catalog_text;
    if (!ReadUtf8(p_catalog_json, catalog_text))
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto document_root = yyjson::read(document_text);
        auto document =
            yyjson::cast<Das::Core::GraphRuntime::Dto::GraphDocumentDto>(
                document_root);

        Das::Core::GraphRuntime::GraphCompiler::ComponentPortSurfaceProvider
            provider;
        if (!catalog_text.empty())
        {
            auto catalog_root = yyjson::read(catalog_text);
            auto surfaces = ParseCatalogPortSurfaces(catalog_root);
            if (!surfaces.has_value())
            {
                return DAS_E_INVALID_ARGUMENT;
            }
            auto surface_map = std::move(*surfaces);
            provider = [surface_map = std::move(surface_map)](
                           const std::string& component_guid)
                -> std::optional<Das::Core::GraphRuntime::GraphCompiler::
                                     ComponentPortSurface>
            {
                auto found = surface_map.find(component_guid);
                if (found == surface_map.end())
                {
                    return std::nullopt;
                }
                return found->second;
            };
        }

        auto boundary =
            Das::Core::GraphRuntime::GraphCompiler::DeriveBoundaryPorts(
                document,
                provider);
        auto boundary_value = yyjson::object(boundary);
        auto serialized = Das::Utils::SerializeYyjsonValue(boundary_value);
        if (!serialized.has_value())
        {
            return DAS_E_FAIL;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            serialized->c_str(),
            pp_out_boundary_json);
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_FAIL;
    }
}

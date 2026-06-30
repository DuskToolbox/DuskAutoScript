#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>

#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/AuthoringDocContract.h>
#include <das/Core/GraphRuntime/FormSequenceProjector.h>
#include <das/Core/GraphRuntime/GraphAuthoring.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/DasJsonCore.h>
#include <cpp_yyjson.hpp>
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
    if (p_compiled_artifact_json)
    {
        const char* utf8 = nullptr;
        DasResult   hr = p_compiled_artifact_json->GetUtf8(&utf8);
        if (DAS::IsFailed(hr) || !utf8)
        {
            last_error_ = "Execute: failed to read compiled artifact JSON";
            DAS_CORE_LOG_ERROR("{}", last_error_);
            return DAS_E_INVALID_POINTER;
        }

        std::string json_str(utf8);
        if (!json_str.empty())
        {
            auto doc = yyjson::read(json_str);
            plan = yyjson::cast<Dto::CompiledGraphPlanDto>(doc);
        }
    }

    // Execute via engine.
    DasResult result = engine_.RunWithHost(
        plan,
        plan.compiled_fingerprint,
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

    // Return an empty result JSON on success.
    return CreateEmptyDasJson(pp_out_result_json);
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
struct GraphAuthoringSessionState
{
    Das::Core::GraphRuntime::Dto::GraphDocumentDto document;
};

namespace
{
    namespace GR = Das::Core::GraphRuntime;
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

    DasResult
    WrapValue(const yyjson::value& value, Das::ExportInterface::IDasJson** out)
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

    /// Read a GraphDocumentDto field-by-field from a JSON object (robust manual
    /// parse at the boundary; aggregate cast would also work but this avoids
    /// throwing on partial input).
    template <typename Obj>
    bool ParseGraphDoc(const Obj& obj, GraphDocDto& out)
    {
        out = GraphDocDto{};
        if (auto v = obj[std::string_view("documentId")].as_string())
        {
            out.document_id = std::string(*v);
        }
        if (auto v = obj[std::string_view("version")].as_int())
        {
            out.version = static_cast<int32_t>(*v);
        }
        if (auto v = obj[std::string_view("fingerprint")].as_string())
        {
            out.fingerprint = std::string(*v);
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
                        out.nodes.push_back(
                            GR::Dto::Detail::CastDtoObject<
                                Das::Core::GraphRuntime::Dto::GraphNodeDto>(nv));
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
                            GR::Dto::Detail::CastDtoObject<
                                Das::Core::GraphRuntime::Dto::GraphEdgeDto>(ev));
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
        return true;
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
        if (obj->contains(std::string_view("items")))
        {
            // formSequence context → project to a linear GraphDocumentDto.
            FormSeqDto seq;
            if (auto v = (*obj)[std::string_view("documentId")].as_string())
            {
                seq.document_id = std::string(*v);
            }
            if (auto v = (*obj)[std::string_view("version")].as_int())
            {
                seq.version = static_cast<int32_t>(*v);
            }
            if (auto v = (*obj)[std::string_view("fingerprint")].as_string())
            {
                seq.fingerprint = std::string(*v);
            }
            auto items = (*obj)[std::string_view("items")].as_array();
            if (items)
            {
                for (const auto& iv : *items)
                {
                    try
                    {
                        seq.items.push_back(
                            GR::Dto::Detail::CastDtoObject<
                                Das::Core::GraphRuntime::Dto::FormSequenceItemDto>(iv));
                    }
                    catch (...)
                    {
                    }
                }
            }
            s.document = GR::FormSequenceProjector::Project(seq);
            s.document.tags.emplace_back(GR::Contract::kLinearTag);
        }
        else if (obj->contains(std::string_view("nodes")))
        {
            // graph context → parse directly.
            ParseGraphDoc(*obj, s.document);
        }
        // else: leave empty (graph-mode) store.
    }

    // -------------------------------------------------------------------
    // Contract change parsing
    // -------------------------------------------------------------------

    template <typename Obj>
    bool ReadStringField(
        const Obj&           obj,
        std::string_view     key,
        std::string&         out)
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

    template <typename Obj>
    std::optional<GR::Contract::GraphNode>
    ParseGraphNode(const Obj& obj)
    {
        GR::Contract::GraphNode node;
        if (!ReadStringField(obj, std::string_view("id"), node.id))
        {
            return std::nullopt;
        }
        ReadStringField(obj, std::string_view("componentGuid"), node.component_guid);
        if (obj.contains(std::string_view("settings")))
        {
            node.settings =
                Das::Utils::CloneYyjsonValue(obj[std::string_view("settings")]);
        }
        return node;
    }

    template <typename Obj>
    std::optional<GR::Contract::GraphConnection>
    ParseGraphConnection(const Obj& obj)
    {
        GR::Contract::GraphConnection c;
        if (!ReadStringField(obj, std::string_view("fromNodeId"), c.from_node_id))
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

    template <typename Obj>
    std::optional<GR::Contract::SequenceItem>
    ParseSequenceItem(const Obj& obj)
    {
        GR::Contract::SequenceItem item;
        if (!ReadStringField(obj, std::string_view("id"), item.id))
        {
            return std::nullopt;
        }
        ReadStringField(obj, std::string_view("type"), item.type);
        if (obj.contains(std::string_view("settings")))
        {
            item.settings =
                Das::Utils::CloneYyjsonValue(obj[std::string_view("settings")]);
        }
        return item;
    }

    bool ParseAuthoringChange(const yyjson::value& json, GR::Contract::AuthoringChange& out)
    {
        out = GR::Contract::AuthoringChange{};
        auto obj = json.as_object();
        if (!obj)
        {
            return false;
        }
        if (!ReadStringField(*obj, std::string_view("op"), out.op))
        {
            return false;
        }

        if (obj->contains(std::string_view("node")))
        {
            if (auto sub = (*obj)[std::string_view("node")].as_object())
            {
                out.node = ParseGraphNode(*sub);
            }
        }
        ReadStringField(*obj, std::string_view("nodeId"), out.node_id);
        if (obj->contains(std::string_view("connection")))
        {
            if (auto sub = (*obj)[std::string_view("connection")].as_object())
            {
                out.connection = ParseGraphConnection(*sub);
            }
        }
        if (obj->contains(std::string_view("settings")))
        {
            out.settings =
                Das::Utils::CloneYyjsonValue((*obj)[std::string_view("settings")]);
        }
        if (obj->contains(std::string_view("item")))
        {
            if (auto sub = (*obj)[std::string_view("item")].as_object())
            {
                out.item = ParseSequenceItem(*sub);
            }
        }
        if (obj->contains(std::string_view("from")))
        {
            if (auto v = (*obj)[std::string_view("from")].as_int())
            {
                out.from = static_cast<std::size_t>(*v);
            }
        }
        if (obj->contains(std::string_view("to")))
        {
            if (auto v = (*obj)[std::string_view("to")].as_int())
            {
                out.to = static_cast<std::size_t>(*v);
            }
        }
        return true;
    }

    /// Build the authoring-result JSON. Per das-yyjson-string-safety: string
    /// members are written with std::make_pair(string_view, copy_string) so the
    /// data is deep-copied into the document arena (no dangling on return).
    yyjson::value BuildChangeResultJson(
        const GR::Contract::AuthoringChangeResult& r,
        int32_t                                revision)
    {
        auto root = Das::Utils::MakeYyjsonObject();
        auto obj  = root.as_object();
        (*obj)[std::string_view("ok")]       = r.Ok();
        (*obj)[std::string_view("revision")] = revision;

        if (!r.Ok())
        {
            std::string_view kind_sv = "InvalidOp";
            switch (r.error_kind)
            {
                case GR::Contract::ChangeErrorKind::InvalidOp:      kind_sv = "InvalidOp"; break;
                case GR::Contract::ChangeErrorKind::NodeNotFound:   kind_sv = "NodeNotFound"; break;
                case GR::Contract::ChangeErrorKind::DuplicateNodeId:kind_sv = "DuplicateNodeId"; break;
                case GR::Contract::ChangeErrorKind::EmptyNodeId:    kind_sv = "EmptyNodeId"; break;
                case GR::Contract::ChangeErrorKind::InvalidEdge:    kind_sv = "InvalidEdge"; break;
                case GR::Contract::ChangeErrorKind::EdgeNotFound:   kind_sv = "EdgeNotFound"; break;
                case GR::Contract::ChangeErrorKind::ItemNotFound:   kind_sv = "ItemNotFound"; break;
                case GR::Contract::ChangeErrorKind::InvalidIndex:   kind_sv = "InvalidIndex"; break;
                case GR::Contract::ChangeErrorKind::NoChange:       kind_sv = "NoChange"; break;
                case GR::Contract::ChangeErrorKind::None:           break;
            }
            (*obj)[std::string_view("errorKind")] =
                std::make_pair(kind_sv, yyjson::copy_string);
            (*obj)[std::string_view("message")] =
                std::make_pair(std::string_view(r.message), yyjson::copy_string);
        }
        return root;
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
        auto* s = new GraphAuthoringSessionState();
        std::string ctx;
        if (ReadUtf8(p_context_json, ctx))
        {
            SeedFromContext(*s, ctx);
        }
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
        // SerializeDocument builds the contract JSON with copy_string so the
        // returned value is self-contained; serialise + reparse into IDasJson.
        auto v   = GR::Contract::SerializeDocument(p->document);
        auto str = Das::Utils::SerializeYyjsonValue(v);
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

        GR::Contract::AuthoringChange change;
        if (change_utf8.empty())
        {
            auto r = WrapValue(
                BuildChangeResultJson(
                    {GR::Contract::ChangeErrorKind::InvalidOp, "empty change request"},
                    p->document.version),
                pp_out_result_json);
            return r;
        }

        auto doc = yyjson::read(change_utf8);
        if (!ParseAuthoringChange(doc, change))
        {
            return WrapValue(
                BuildChangeResultJson(
                    {GR::Contract::ChangeErrorKind::InvalidOp, "malformed change request"},
                    p->document.version),
                pp_out_result_json);
        }

        auto          r = GR::Contract::ApplyAuthoringChange(p->document, change);
        return WrapValue(BuildChangeResultJson(r, p->document.version),
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
        GR::GraphCompiler compiler;
        auto              plan = compiler.Compile(p->document);
        return WrapValue(yyjson::object(plan), pp_out_compiled_plan_json);
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

DAS_C_API DasResult GraphAuthoringSessionUpgradeToGraph(
    GraphAuthoringSessionState* p)
{
    if (p == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    return GR::Contract::UpgradeToGraph(p->document) ? DAS_S_OK : DAS_E_FAIL;
}


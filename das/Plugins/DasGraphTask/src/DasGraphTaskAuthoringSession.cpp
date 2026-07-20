#define DAS_BUILD_SHARED

#include "DasGraphTaskAuthoringSession.h"

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>

#include <string>
#include <string_view>
#include <utility>

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    namespace
    {
        // -------------------------------------------------------------------
        // IDasJson <-> UTF-8 string marshaling (the Core C ABI speaks JSON
        // strings; the authoring-session COM boundary speaks IDasJson).
        // -------------------------------------------------------------------

        std::string JsonToUtf8(ExportInterface::IDasJson* json)
        {
            if (json == nullptr)
            {
                return {};
            }
            DasPtr<IDasReadOnlyString> text;
            if (DAS::IsFailed(json->ToString(0, text.Put()))
                || text.Get() == nullptr)
            {
                return {};
            }
            const char* utf8 = nullptr;
            if (DAS::IsFailed(text->GetUtf8(&utf8)) || utf8 == nullptr)
            {
                return {};
            }
            return std::string(utf8);
        }

        DasPtr<IDasReadOnlyString> MakeReadOnlyString(const std::string& s)
        {
            DasPtr<IDasReadOnlyString> ro;
            CreateIDasReadOnlyStringFromUtf8(s.c_str(), ro.Put());
            return ro;
        }

        // Phase 84 actionCatalog wiring (D-84-31/32): the Provider must feed
        // the session the registered IDasTaskComponent component_guid set so
        // the Core can (a) emit actionCatalog.entries[] on GetDocument and
        // (b) validate actionId on addGraphNode/addFormSequenceItem.
        //
        // The plugin cannot reach PluginManager directly (it is loaded BY
        // PluginManager and only receives an IDasTaskComponentHost* which does
        // not expose enumeration). The Scheduler / host process that creates
        // the session context is the natural owner of the registry snapshot;
        // it injects the catalog as a top-level `actionCatalog` JSON array of
        // strings inside the context document. The plugin forwards that array
        // to the Core via GraphAuthoringSessionSetActionCatalog.
        //
        // Context shape (Phase 84 cutover):
        //   {"properties": <GraphDocumentDto>,
        //    "actionCatalog": ["{guid-a}", "{guid-b}", ...]}
        // When `actionCatalog` is absent the session falls back to an empty
        // catalog; addGraphNode/addFormSequenceItem then reject every
        // actionId as UnknownActionId until a future wave (84-03) replaces
        // MergeTaskComponentCatalog and the Scheduler starts injecting the
        // registry-derived array on every session creation.
        //
        // Per das-yyjson-string-safety: every std::string payload written to
        // yyjson here is a string_view literal or a value extracted by yyjson
        // itself; no lvalue std::string is assigned.
        void WireActionCatalogFromContext(
            GraphAuthoringSessionState* state,
            const std::string&          context_json)
        {
            if (state == nullptr || context_json.empty())
            {
                return;
            }
            yyjson::value root;
            try
            {
                root = yyjson::read(context_json);
            }
            catch (...)
            {
                return;
            }
            auto obj = root.as_object();
            if (!obj || !obj->contains(std::string_view("actionCatalog")))
            {
                return;
            }
            // Re-serialise the catalog array as a flat JSON array of strings
            // (the form GraphAuthoringSessionSetActionCatalog expects) and
            // hand it to the Core.
            auto arr = (*obj)[std::string_view("actionCatalog")].as_array();
            if (!arr)
            {
                return;
            }
            std::string flat = "[";
            bool        first = true;
            for (const auto& v : *arr)
            {
                auto s = v.as_string();
                if (!s)
                {
                    continue;
                }
                if (!first)
                {
                    flat += ',';
                }
                flat += '"';
                flat += *s;
                flat += '"';
                first = false;
            }
            flat += "]";
            auto catalog_str = MakeReadOnlyString(flat);
            GraphAuthoringSessionSetActionCatalog(state, catalog_str.Get());
        }
    } // namespace

    DasGraphTaskAuthoringSession::DasGraphTaskAuthoringSession(
        std::string context_json)
    {
        auto ctx_str = MakeReadOnlyString(context_json);
        // CreateGraphAuthoringSession seeds the Core-owned GraphDocumentDto
        // store from the context (formSequence / graph / empty).
        CreateGraphAuthoringSession(ctx_str.Get(), &state_);
        // Phase 84 actionCatalog wiring (D-84-31/32/33): inject the registry
        // snapshot the Scheduler included in the context so GetDocument emits
        // actionCatalog and addGraphNode/addFormSequenceItem can validate
        // actionId.
        WireActionCatalogFromContext(state_, context_json);
    }

    DasGraphTaskAuthoringSession::~DasGraphTaskAuthoringSession()
    {
        DestroyGraphAuthoringSession(state_);
        state_ = nullptr;
    }

    DasResult DasGraphTaskAuthoringSession::GetDocument(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_document_json)
    {
        if (pp_out_document_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_document_json = nullptr;

        auto req_str = MakeReadOnlyString(JsonToUtf8(p_request_json));
        return GraphAuthoringSessionGetDocument(
            state_,
            req_str.Get(),
            pp_out_document_json);
    }

    DasResult DasGraphTaskAuthoringSession::ApplyChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        auto change_str = MakeReadOnlyString(JsonToUtf8(p_request_json));
        return GraphAuthoringSessionApplyChange(
            state_,
            change_str.Get(),
            pp_out_result_json);
    }

    DasResult DasGraphTaskAuthoringSession::Compile(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        auto req_str = MakeReadOnlyString(JsonToUtf8(p_request_json));
        return GraphAuthoringSessionCompile(
            state_,
            req_str.Get(),
            pp_out_result_json);
    }
} // namespace Plugins::DasGraphTask
DAS_NS_END

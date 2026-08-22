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

        // actionCatalog wiring: the Provider must feed the
        // session the registered IDasTaskComponent catalog so the Core can
        // (a) emit actionCatalog.entries[] (with manifest kind labels) on
        // GetDocument and (b) validate actionId on
        // addGraphNode/addFormSequenceItem.
        //
        // The plugin cannot reach PluginManager directly (it is loaded BY
        // PluginManager and only receives an IDasTaskComponentHost* which does
        // not expose enumeration). The Scheduler / host process that creates
        // the session context is the natural owner of the registry snapshot;
        // it injects the catalog as a top-level `actionCatalog` JSON array of
        // {actionId,label} objects inside the context document. The plugin
        // re-serialises that array unchanged and forwards it to the Core via
        // GraphAuthoringSessionSetActionCatalog — entry shape validation and
        // the missing-label fallback live in the Core.
        //
        // Context shape:
        //   {"properties": <GraphDocumentDto>,
        //    "actionCatalog": [{"actionId":"{guid}","label":"kind"}, ...]}
        // When `actionCatalog` is absent the session keeps an empty catalog;
        // addGraphNode/addFormSequenceItem then reject every actionId as
        // UnknownActionId.
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
            auto arr = (*obj)[std::string_view("actionCatalog")].as_array();
            if (!arr)
            {
                return;
            }
            // Forward the catalog array as-is (object entries survive
            // end-to-end; no flattening back to guid strings).
            auto catalog_serialized = Das::Utils::SerializeYyjsonValue(
                (*obj)[std::string_view("actionCatalog")]);
            if (!catalog_serialized)
            {
                return;
            }
            auto catalog_str = MakeReadOnlyString(*catalog_serialized);
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

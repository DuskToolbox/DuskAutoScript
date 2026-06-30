#define DAS_BUILD_SHARED

#include "DasGraphTaskAuthoringSession.h"

#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>

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
            if (DAS::IsFailed(json->ToString(0, text.Put())) || text.Get() == nullptr)
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
    } // namespace

    DasGraphTaskAuthoringSession::DasGraphTaskAuthoringSession(
        std::string context_json)
    {
        auto ctx_str = MakeReadOnlyString(context_json);
        // CreateGraphAuthoringSession seeds the Core-owned GraphDocumentDto
        // store from the context (formSequence / graph / empty).
        CreateGraphAuthoringSession(ctx_str.Get(), &state_);
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
            state_, req_str.Get(), pp_out_document_json);
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
            state_, change_str.Get(), pp_out_result_json);
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
            state_, req_str.Get(), pp_out_result_json);
    }
} // namespace Plugins::DasGraphTask
DAS_NS_END

#define DAS_BUILD_SHARED

#include "DasGraphTaskAuthoringSessionFactory.h"

#include "DasGraphTaskAuthoringSession.h"

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>

#include <new>
#include <utility>

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    namespace
    {
        /// Read the context document JSON (a formSequence doc, a graph doc, or
        /// empty) for the session to seed its Core-owned store from.
        std::string InitialSequenceFromContext(ExportInterface::IDasJson* context)
        {
            if (context == nullptr)
            {
                return {};
            }
            DasPtr<IDasReadOnlyString> text;
            if (DAS::IsFailed(context->ToString(0, text.Put())) || text.Get() == nullptr)
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
    } // namespace

    DasResult DasGraphTaskAuthoringSessionFactory::CreateSession(
        const DasGuid& /*task_guid*/,
        ExportInterface::IDasJson*                  p_context_json,
        PluginInterface::IDasTaskAuthoringSession** pp_out_session)
    {
        if (pp_out_session == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_session = nullptr;

        try
        {
            auto initial = InitialSequenceFromContext(p_context_json);
            auto* session =
                new DasGraphTaskAuthoringSession(std::move(initial));
            session->AddRef();
            *pp_out_session = session;
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }
} // namespace Plugins::DasGraphTask
DAS_NS_END

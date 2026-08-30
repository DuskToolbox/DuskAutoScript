#define DAS_BUILD_SHARED

#include "MaapiAuthoringSessionFactory.h"

#include "MaapiAuthoringSession.h"
#include "PluginUtils.h"

#include <das/Plugins/DasMaaPi/PiParser.h>

#include <new>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    DasResult MaapiAuthoringSessionFactory::CreateSession(
        const DasGuid&,
        ExportInterface::IDasJson*                  p_context_json,
        PluginInterface::IDasTaskAuthoringSession** pp_out_session)
    {
        if (!pp_out_session)
        {
            return DAS_E_INVALID_POINTER;
        }
        AcceptedSettingsDto settings;
        int64_t             revision = 0;
        if (auto context = ReadJson(p_context_json))
        {
            settings = ParseAcceptedSettings(*context);
            // SchedulerService::MakeAuthoringContextJson 把权威 revision
            // (task_json.authoring.revision) 放进 context 顶层。读出来初始化
            // session，避免 GetDocument 返回 stale revision (handoff2 Bug 2)。
            const auto obj = context->as_object();
            if (obj && obj->contains(std::string_view("revision")))
            {
                if (auto rev = (*obj)[std::string_view("revision")].as_sint())
                {
                    revision = *rev;
                }
            }
        }

        auto* session =
            new MaapiAuthoringSession(std::move(settings), revision);
        session->AddRef();
        *pp_out_session = session;
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END

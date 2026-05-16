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
        if (auto context = ReadJson(p_context_json))
        {
            settings = ParseAcceptedSettings(*context);
        }

        auto* session = new MaapiAuthoringSession(std::move(settings));
        session->AddRef();
        *pp_out_session = session;
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END

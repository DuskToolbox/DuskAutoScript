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
        bool                component_context = false;
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
            // 图内节点下钻编辑（IDasTaskComponent 语境）标记：engaged-only
            // （键缺席/非布尔一律 false = standalone 行为逐字节一致）。true
            // 时投影器生成暴露开关伴生 bool 字段行与 ports section 推导；
            // 暴露状态仅 component 语境可写。读法与 revision 通道同款
            // （contains + 类型安全 as_bool）。
            if (obj && obj->contains(std::string_view("componentContext")))
            {
                if (auto flag =
                        (*obj)[std::string_view("componentContext")].as_bool())
                {
                    component_context = *flag;
                }
            }
        }

        auto* session = new MaapiAuthoringSession(
            std::move(settings),
            revision,
            component_context);
        session->AddRef();
        *pp_out_session = session;
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END

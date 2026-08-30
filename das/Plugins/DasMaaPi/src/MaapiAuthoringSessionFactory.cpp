#define DAS_BUILD_SHARED

#include "MaapiAuthoringSessionFactory.h"

#include "MaapiAuthoringSession.h"
#include "PluginUtils.h"

#include <das/Plugins/DasMaaPi/PiParser.h>

#include <new>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
    {
        // Scheduler 注入 context 顶层 actionCatalog 的组件注册表快照（与
        // graph 任务同一条通道）：条目 {actionId,label,ports?,config?}，此处
        // 取公共 wire 两键，内部通道字段（ports/config）属 graph 消费面不透
        // 传。缺 actionId/非对象条目跳过；缺 label 回退 actionId（Core 校验
        // 器要求 label 非空——与调度器 CatalogEntryLabel 的兜底口径一致）。
        std::vector<RegistryActionCatalogEntry> ParseRegistryActionCatalog(
            const yyjson::value& context)
        {
            std::vector<RegistryActionCatalogEntry> entries;
            auto context_object_opt = context.as_object();
            if (!context_object_opt
                || !context_object_opt->contains(
                    std::string_view("actionCatalog")))
            {
                return entries;
            }
            auto catalog_array_opt =
                (*context_object_opt)[std::string_view("actionCatalog")]
                    .as_array();
            if (!catalog_array_opt)
            {
                return entries;
            }
            for (const auto& item : *catalog_array_opt)
            {
                auto item_object_opt = item.as_object();
                if (!item_object_opt
                    || !item_object_opt->contains(std::string_view("actionId")))
                {
                    continue;
                }
                auto action_id =
                    (*item_object_opt)[std::string_view("actionId")]
                        .as_string();
                if (!action_id || action_id->empty())
                {
                    continue;
                }
                RegistryActionCatalogEntry entry;
                entry.action_id = std::string(*action_id);
                entry.label = entry.action_id;
                if (item_object_opt->contains(std::string_view("label")))
                {
                    if (auto label =
                            (*item_object_opt)[std::string_view("label")]
                                .as_string();
                        label && !label->empty())
                    {
                        entry.label = std::string(*label);
                    }
                }
                entries.push_back(std::move(entry));
            }
            return entries;
        }
    } // namespace

    DasResult MaapiAuthoringSessionFactory::CreateSession(
        const DasGuid&,
        ExportInterface::IDasJson*                  p_context_json,
        PluginInterface::IDasTaskAuthoringSession** pp_out_session)
    {
        if (!pp_out_session)
        {
            return DAS_E_INVALID_POINTER;
        }
        AcceptedSettingsDto                     settings;
        int64_t                                 revision = 0;
        std::vector<RegistryActionCatalogEntry> registry_action_catalog;
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
            registry_action_catalog = ParseRegistryActionCatalog(*context);
        }

        auto* session = new MaapiAuthoringSession(
            std::move(settings),
            revision,
            std::move(registry_action_catalog));
        session->AddRef();
        *pp_out_session = session;
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END

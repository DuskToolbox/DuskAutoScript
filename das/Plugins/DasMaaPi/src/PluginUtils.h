#ifndef DAS_PLUGINS_DASMAAPI_PLUGINUTILS_H
#define DAS_PLUGINS_DASMAAPI_PLUGINUTILS_H

#include "PiUpdateService.h"
#include <das/Plugins/DasMaaPi/AdbDeviceFinder.h>

#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Plugins/DasMaaPi/AuthoringProjector.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>
#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/TaskComponentPorts.h>
#include <das/_autogen/idl/abi/IDasI18n.h>

#include <map>
#include <optional>
#include <vector>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    DasPtr<ExportInterface::IDasJson> WrapJson(yyjson::value value);
    yyjson::value                     JsonString(std::string_view value);
    std::optional<yyjson::value>      ReadJson(ExportInterface::IDasJson* json);

    /// 插件级 i18n 惰性单例：<plugin_dir>/i18n 经 this_line_location 推导
    /// （与 MaaFW LogDir 同范式）。目录缺失 / 装载失败永久降级（返回
    /// nullptr，调用方回退硬编码文案，不重试）。
    ExportInterface::IDasI18n* GetPluginI18n();

    /// 经 i18n 翻译 key；i18n 为空 / key miss 返回空串（调用方回退硬编码）。
    std::string TranslatePluginKey(const char* key);

    std::optional<PiCatalog> TryParseCatalog(
        const AcceptedSettingsDto&    settings,
        std::vector<PiDiagnosticDto>& diagnostics);

    // 更新五字段与 applyUpdate 按钮态由进程级 UpdateCheckStore 的 per-pi_dir
    // 条目驱动（后端状态驱动投影，前端只按投影渲染；无条目 → 五字段不投影 +
    // applyUpdate disabled）。device_cache 供 adbDevice 字段投影 candidates
    // （discoverAdbDevices 命令填充，session 内存态不落盘）。空指针 →
    // candidates 空数组但字段仍 emit。
    yyjson::value BuildDocument(
        const AcceptedSettingsDto&        settings,
        int64_t                           revision,
        std::vector<PiDiagnosticDto>&     diagnostics,
        const std::vector<AdbDeviceInfo>* device_cache = nullptr);

    // project_document=false 时跳过 document 投影（uiEvent 等 UI 事件响应不带
    // document，避免无谓 catalog 解析 + 投影；前端据此分流——有 document 重建，
    // 无则只更新 revision）。command_result 非空时向 envelope 追加可选
    // commandResult={commandId, data}（通用业务结果回传机制，checkUpdate 用它
    // 返回 progress_key；无业务结果的命令不带）。
    yyjson::value BuildApplyResult(
        const AcceptedSettingsDto&        settings,
        int64_t                           revision,
        std::vector<PiDiagnosticDto>      diagnostics,
        bool                              project_document = true,
        const yyjson::value*              command_result = nullptr,
        const std::vector<AdbDeviceInfo>* device_cache = nullptr);

    // Port derivation utilities (D-05/D-06/D-07)

    /// Derive graph port definitions from PI catalog for a given task (D-05).
    std::vector<Core::GraphRuntime::Dto::GraphPortDefinitionDto>
    DerivePortDefinitions(
        const PiCatalog&   catalog,
        const std::string& task_name);

    /// Build graph_port_id → pi_param_name mapping table (D-06).
    std::map<std::string, std::string> DerivePortMap(
        const PiCatalog&   catalog,
        const std::string& task_name);

    /// Serialize port definitions to a yyjson JSON array (D-07).
    yyjson::value BuildPortDefinitionsJson(
        const std::vector<Core::GraphRuntime::Dto::GraphPortDefinitionDto>&
            ports);

} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_PLUGINUTILS_H

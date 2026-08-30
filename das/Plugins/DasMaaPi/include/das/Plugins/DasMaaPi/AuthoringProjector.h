#pragma once

#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/AdbDeviceFinder.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>

#include <optional>

namespace Das::ExportInterface
{
    struct IDasProgressService;
}

namespace Das::Plugins::DasMaaPi
{
    struct PiUpdateInfo;
    class PiHttpClient;
    class MaapiAuthoringSession;
    struct RegistryActionCatalogEntry;

    // invokeCommand 命令业务结果回传结构。各分支自填 command_result；
    // 调用方（MaapiAuthoringSession::ApplyChange）按 engaged 状态塞
    // envelope。零 T* 出参指针 — session 的 device_cache_ 通过
    // MaapiAuthoringSession& 引用直读直写，符合 DAS DasRef/DasOutPtr
    // 全量替换 T** 出参的风格。
    struct InvokeCommandResult
    {
        bool                         ok = false;
        std::optional<yyjson::value> command_result;
    };

    AcceptedSettingsDto ParseAcceptedSettings(const yyjson::value& value);
    yyjson::value       SerializeAcceptedSettings(
        const AcceptedSettingsDto& settings);

    // 更新五字段（updateVersion 等）与 applyUpdate 按钮态由进程级
    // UpdateCheckStore 的 per-pi_dir 条目驱动（checkUpdate 结果真相源）：
    // 无条目 → 五字段不投影 + applyUpdate disabled；条目四态派生见投影
    // 实现。后端状态驱动 commands 投影，前端只按投影渲染，业务中立。
    //
    // device_cache 为空指针时 adbDevice.candidates 投影为空（前端 dropdown 空
    // 候补但字段仍 emit）；非空时按 cache 内容构造 {value=address, label=name}
    // 候补。cache 是 session 内存态，不进 acceptedProperties 不落盘。
    yyjson::value ProjectAuthoringDocument(
        const AcceptedSettingsDto&          settings,
        const PiCatalog*                    catalog,
        const std::vector<PiDiagnosticDto>& diagnostics,
        int64_t                             revision,
        std::vector<PiDiagnosticDto>*       projection_diagnostics = nullptr,
        const std::vector<AdbDeviceInfo>*   device_cache = nullptr);

    void ApplySetValueChange(
        AcceptedSettingsDto& settings,
        const yyjson::value& change,
        const PiCatalog*     catalog = nullptr);

    void ApplyAddSequenceItem(
        AcceptedSettingsDto& settings,
        const PiCatalog*     catalog,
        std::string_view     task_name,
        bool                 enabled = true);

    void ApplyMoveSequenceItem(AcceptedSettingsDto& settings, int from, int to);

    void ApplyRemoveSequenceItem(
        AcceptedSettingsDto& settings,
        std::string_view     task_name);

    // D-84-07: 严格 Phase 84 change dispatch。每个 handler 严格校验 payload，
    // 成功时修改 settings 并返回 true；失败时向 diagnostics 追加一条
    // {severity:"error", int_code?, message, path?} 诊断并返回 false（调用
    // 方据此放弃 candidate、不递增 revision）。payload 是 change 的 payload
    // 对象（不含 baseRevision/kind 外层）。
    bool ApplyStrictSetValueChange(
        AcceptedSettingsDto&          settings,
        const yyjson::value&          payload,
        const PiCatalog*              catalog,
        std::vector<PiDiagnosticDto>& diagnostics);

    bool ApplyAddFormSequenceItem(
        AcceptedSettingsDto&          settings,
        const PiCatalog*              catalog,
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics,
        const std::vector<RegistryActionCatalogEntry>* registry_entries);

    bool ApplyMoveFormSequenceItem(
        AcceptedSettingsDto&          settings,
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics);

    bool ApplyRemoveFormSequenceItem(
        AcceptedSettingsDto&          settings,
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics);

    // invokeCommand 严格 dispatch。返回 InvokeCommandResult {ok,
    // command_result}，零 T* 出参指针 — device_cache_ 经 session 引用直读
    // 直写；checkUpdate/applyUpdate 的检查结果经进程级 UpdateCheckStore
    // 传递（不再有 session 暂态）。已知 commandId：
    //   - "newPreset"（通用 invokeCommand 落地，command_result nullopt）
    //   - "checkUpdate"（受理成功 → command_result 自填
    //     {commandId:"checkUpdate", data:{progressKey}}；否则 nullopt）
    //   - "applyUpdate"（受理成功 → command_result 自填
    //     {commandId:"applyUpdate", data:{accepted:true}}）
    //   - "discoverAdbDevices"（扫描设备填 session.device_cache_，
    //     command_result nullopt — 设备进 adbDevice field candidates 不走
    //     commandResult.data）
    // 未知 commandId 返回 {ok=false} 并 emit error 诊断（DAS_E_NOT_FOUND）。
    // http_seam 为空时构造 BeastHttpClient（生产）；测试注入 FakePiHttpClient。
    // progress_service_seam 为空时 applyUpdate 路径经 GetCurrentIpcContext
    // Resolve 主进程 service（测试注入 fake）。
    InvokeCommandResult ApplyInvokeCommandChange(
        MaapiAuthoringSession&                     session,
        AcceptedSettingsDto&                       settings,
        const yyjson::value&                       payload,
        std::vector<PiDiagnosticDto>&              diagnostics,
        PiHttpClient*                              http_seam = nullptr,
        Das::ExportInterface::IDasProgressService* progress_service_seam =
            nullptr,
        // discoverAdbDevices 扫描入口。生产留空（走 DefaultAdbDeviceEnumerator
        // 经 MaaToolkit 扫描真机）；测试传 lambda 注入 0/1/N 设备。与 http_seam
        // 同为显式依赖注入参数——业务代码不持有可变全局 test seam。
        AdbDeviceEnumerator adb_enumerator = {});

    // D-84-29: convertView 在 MaaPI 一律拒绝（MaaPI 只支持 formSequence）。
    // payload 严格 {targetViewKind}；返回 false 并 emit error 诊断。
    bool ApplyConvertViewChange(
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics);

    // uiEvent kind handler：通用 UI 事件 → ui.values kv（视图偏好 +
    // 插件自定义）。 payload {type, target, data}（OnlyKeysExact
    // 外层严格；target/data 按 type）； type 驱动映射（当前
    // sectionExpandChanged → values["sectionExpand.<id>"]）。 不参与 PI
    // 业务语义；ApplyChange 主流程对 uiEvent 短路 document（不投影）。
    bool ApplyUiEventChange(
        AcceptedSettingsDto&          settings,
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics);

    void ApplyPreset(
        AcceptedSettingsDto& settings,
        const PiCatalog&     catalog,
        std::string_view     preset_name);

    // D-84-40: 为新 task 实例分配 Provider 颁发的稳定 item_id。
    // 基于 atomic counter 生成 "item_<n>" 形式，避免与已存在 id 冲突。
    // 不从 taskName/actionId/index/label 派生，避免同 task 类型撞 id。
    std::string AllocateItemId(const AcceptedSettingsDto& settings);
} // namespace Das::Plugins::DasMaaPi

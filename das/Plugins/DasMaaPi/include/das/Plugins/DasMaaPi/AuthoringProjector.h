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
    // D-89-03: 更新检查暂态（checkUpdate 结果 + applyUpdate 输入）。跨
    // session 持久化的"待更新"状态不存在——pending_update_ 是
    // MaapiAuthoringSession 内存暂态，不入 acceptedProperties、不落盘（Phase 87
    // 黑盒约束）。
    struct PiUpdateInfo;
    class PiHttpClient;
    class MaapiAuthoringSession;

    // invokeCommand 命令业务结果回传结构。各分支自填 command_result；
    // 调用方（MaapiAuthoringSession::ApplyChange）按 engaged 状态塞
    // envelope。零 T* 出参指针 — session 的 pending_update_/device_cache_
    // 通过 MaapiAuthoringSession& 引用直读直写，符合 DAS DasRef/DasOutPtr
    // 全量替换 T** 出参的风格。
    struct InvokeCommandResult
    {
        bool                         ok = false;
        std::optional<yyjson::value> command_result;
    };

    AcceptedSettingsDto ParseAcceptedSettings(const yyjson::value& value);
    yyjson::value       SerializeAcceptedSettings(
        const AcceptedSettingsDto& settings);

    // pending_update 为空指针（或未 engaged）时 applyUpdate 命令投影 disabled。
    // 后端状态驱动 commands 投影（Open Question 1 决策）；前端只按投影
    // disabled 字段渲染，业务中立。
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
        const std::optional<PiUpdateInfo>*  pending_update = nullptr,
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
        std::vector<PiDiagnosticDto>& diagnostics);

    bool ApplyMoveFormSequenceItem(
        AcceptedSettingsDto&          settings,
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics);

    bool ApplyRemoveFormSequenceItem(
        AcceptedSettingsDto&          settings,
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics);

    // invokeCommand 严格 dispatch。返回 InvokeCommandResult {ok,
    // command_result}，零 T* 出参指针 — pending_update_/device_cache_ 经
    // session 引用直读直写。已知 commandId：
    //   - "newPreset"（通用 invokeCommand 落地，command_result nullopt）
    //   - "checkUpdate"（成功且有 pending progress_key → command_result 自填
    //     {commandId:"checkUpdate", data:{progressKey}}；否则 nullopt）
    //   - "applyUpdate"（成功清 pending；command_result nullopt）
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

    // 为 settings 内所有空 item_id 的 task 实例补齐稳定 item_id。仅补空，
    // 已有 item_id 原样保留。覆盖 flat pi.tasks。调用幂等：同一 settings
    // 多次调用结果稳定。
    // 用途：parse 后兜底 (legacy pre-84 输入) + apply 路径兜底 (避免任何
    // apply mutation 留下空 item_id 导致投影层 emit 空 id，触发前端
    // "FormSequenceItem.id must be non-empty")。
    void NormalizeItemIds(AcceptedSettingsDto& settings);
} // namespace Das::Plugins::DasMaaPi

#pragma once

#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/PiCatalog.h>

#include <optional>

namespace Das::Plugins::DasMaaPi
{
    AcceptedSettingsDto ParseAcceptedSettings(const yyjson::value& value);
    yyjson::value       SerializeAcceptedSettings(
        const AcceptedSettingsDto& settings);

    yyjson::value ProjectAuthoringDocument(
        const AcceptedSettingsDto&          settings,
        const PiCatalog*                    catalog,
        const std::vector<PiDiagnosticDto>& diagnostics,
        int64_t                             revision,
        std::vector<PiDiagnosticDto>*       projection_diagnostics = nullptr);

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

    // D-84-28: invokeCommand 严格 dispatch。已知 commandId：
    //   - "newPreset"（D-84.2-04 通用 invokeCommand 落地）
    // 未知 commandId 返回 false 并 emit error 诊断。
    bool ApplyInvokeCommandChange(
        AcceptedSettingsDto&          settings,
        const yyjson::value&          payload,
        std::vector<PiDiagnosticDto>& diagnostics);

    // D-84-29: convertView 在 MaaPI 一律拒绝（MaaPI 只支持 formSequence）。
    // payload 严格 {targetViewKind}；返回 false 并 emit error 诊断。
    bool ApplyConvertViewChange(
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

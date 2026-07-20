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
    //   - "newConfiguration" / "copyConfiguration"（84-05 已实现）
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

    // D-84-28/42: invokeCommand 分派。已知 commandId：
    //   - "newConfiguration":  创建空 configuration + 新 id/name，切为选中
    //   - "copyConfiguration": 拷贝当前选中 configuration，重新生成配置与
    //                          每个 task 的 item_id，保留用户值/顺序，切为选中
    // 未知 commandId 静默忽略 (84-07 owns full Apply cutover)。
    void ApplyInvokeCommand(
        AcceptedSettingsDto& settings,
        std::string_view     command_id);

    // D-84-40: 为新 task 实例分配 Provider 颁发的稳定 item_id。
    // 基于 atomic counter 生成 "item_<n>" 形式，避免与已存在 id 冲突。
    // 不从 taskName/actionId/index/label 派生，避免同 task 类型撞 id。
    std::string AllocateItemId(const AcceptedSettingsDto& settings);

    // D-84-42: 为新 configuration 实例分配稳定 id 与本地化名称。
    // id 基于 atomic counter 生成 "cfg_<n>"；name 自动生成 (命令无参数)。
    std::string AllocateConfigurationId(const AcceptedSettingsDto& settings);
    std::string AllocateConfigurationName(const AcceptedSettingsDto& settings);

    // 取当前选中 configuration 的可变 / 只读指针。
    // 若 configurations[] 为空或 selected_configuration_id 不匹配，返回
    // nullptr，调用方应回退到 flat 字段 (legacy 兼容路径)。
    MaapiConfigurationDto* GetCurrentConfiguration(
        AcceptedSettingsDto& settings);
    const MaapiConfigurationDto* GetCurrentConfiguration(
        const AcceptedSettingsDto& settings);

    // D-84-41/42: flat pi 字段与 configurations[selected] 双向同步。
    // Apply 修改 flat 字段后，调用 SyncCurrentConfigFromActive 把 flat 写回
    // configurations[selected]，再调用 SerializeAcceptedSettings 即可输出
    // 一致的 configurations[]。配置为空 (legacy fixture) 时是 no-op。
    void SyncCurrentConfigFromActive(AcceptedSettingsDto& settings);
    void SyncActiveFromCurrentConfig(AcceptedSettingsDto& settings);
} // namespace Das::Plugins::DasMaaPi

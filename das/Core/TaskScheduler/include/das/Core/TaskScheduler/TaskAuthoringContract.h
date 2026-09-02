#pragma once

#include <cassert>
#include <cpp_yyjson.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Das::Core::TaskScheduler
{

    /// 批量 apply 请求的 changes 数量上限。HTTP 请求体是外部输入，数组
    /// 直接放大 per-session 写锁内的逐 change 循环，无界即 DoS 面；256
    /// 覆盖 UI 编排（extract 框选等复合操作）的最大合理批量。
    inline constexpr std::size_t kMaxAuthoringApplyChanges = 256;

    struct AuthoringValidationResult
    {
        bool        valid = false;
        std::string error_kind;
        std::string message;
    };

    // Guard error returned by SchedulerService lock-held authoring helpers
    // (CheckSchedulerIdleLocked / ResolveAuthoringCapabilityLocked). The
    // std::optional<AuthoringGuardError>::engaged state expresses "guard
    // failed" so no separate valid flag is needed.
    struct AuthoringGuardError
    {
        std::string error_kind;
        std::string message;
    };

    AuthoringValidationResult ValidateAuthoringDocument(
        const yyjson::value& document);

    /// 批量 apply 请求校验：{baseRevision, changes:[{kind,payload},...]}。
    /// changes 必须是非空数组且元素数 ≤ kMaxAuthoringApplyChanges；每元素
    /// 走既有单 change 的 kind/payload 严格校验。旧的单 change 顶层形态
    /// （顶层 kind/payload 键）被 OnlyKeys 拒绝——全量迁移 fail-closed。
    AuthoringValidationResult ValidateAuthoringChange(
        const yyjson::value& change);

    // D-84-08: Provider Get result envelope `{document?, diagnostics[]}`.
    // `document` is optional; `diagnostics` is required (may be empty).
    AuthoringValidationResult ValidateAuthoringGetResult(
        const yyjson::value& result);

    // D-84-08/D-84-10: provider Apply result envelope
    // `{acceptedProperties, document, diagnostics[]}`. The result must carry
    // non-null `acceptedProperties`, a valid `document`, and a strict
    // `diagnostics` array. `kind` names the applied change: the viewData
    // lightweight channel ("moveGraphNode"/"moveGraphNodes") does not bump
    // the semantic revision, so its success envelope must keep
    // `document.revision == baseRevision`; every other kind still requires
    // `baseRevision + 1` on success. The default keeps existing callers on
    // the semantic path (the batch pipeline validates each change
    // individually against its own iteration base).
    AuthoringValidationResult ValidateAuthoringApplyResult(
        const yyjson::value& result,
        int64_t              baseRevision,
        std::string_view     kind = {});

    // 批量 apply 的整批结果不变量：成功域为
    // [base + (存在 bump change ? 1 : 0), base + bump change 数]（每个语义
    // change 恰 +1、viewData 轻量通道 0）；失败 envelope（有 error
    // diagnostic）恒 == base（批内零突变）。diagnostics 由 pipeline 聚合，
    // 此校验只约束 envelope 形状与 revision 域。
    AuthoringValidationResult ValidateAuthoringApplyResult(
        const yyjson::value&            result,
        int64_t                         baseRevision,
        const std::vector<std::string>& kinds);

    // D-84-08: provider Compile result envelope `{compile, diagnostics[]}`.
    AuthoringValidationResult ValidateAuthoringCompileResult(
        const yyjson::value& result);

    yyjson::value MakeAuthoringError(
        std::string_view error_kind,
        std::string_view message);

    // Diagnostic-array inspection helpers. Used by the apply pipeline to
    // distinguish a provider-rejected apply (error diagnostics present) from a
    // malformed envelope, so the real rejection reason surfaces instead of a
    // generic revisionMismatch. Defined in TaskAuthoringContract.cpp.
    bool        HasErrorDiagnostic(const yyjson::value& diagnostics_array);
    std::string FirstErrorDiagnosticMessage(
        const yyjson::value& diagnostics_array);

} // namespace Das::Core::TaskScheduler

#pragma once

#include "PiUpdateService.h"

#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasI18n.h>
#include <das/_autogen/idl/abi/IDasProgressService.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    // DasMaaPi 侧推送链路核心。持有 SSE 路由 key + 主进程
    // IDasProgressService proxy；Start() 注册本地 IDasProgressCallback（经
    // SerializeInInterfaceParam 自动代理化），Report() 把 UpdateProgress 装配成
    // IDasProgressUpdate 字段增量上报（复用 setValue payload 字段语义，
    // 无 percent/status/stage——显示由 document 字段值决定），Stop()
    // RemoveCallback 释放 proxy（成对生命周期）。
    // reporter 生命周期被跨进程 callback 反向引用共享（callback proxy 由
    // DasHttp 保活、可能活得比 reporter 久），故 reporter 由 shared_ptr 管理、
    // callback 持 weak_ptr（lock() 判空后访问，杜绝 owner 悬垂）。Start() 用
    // shared_from_this()，要求创建方必须 make_shared。
    class DasProgressReporter final
        : public std::enable_shared_from_this<DasProgressReporter>
    {
    public:
        DasProgressReporter(
            std::string                                       key,
            DasPtr<Das::ExportInterface::IDasProgressService> service);

        // 注册 callback（AddCallback）。失败返回 false（service 不可用）。
        bool Start();
        // 反注册 callback（RemoveCallback）+ 释放 proxy。幂等。
        void Stop();
        // RAII：析构自动 Stop()，异常路径（ApplyUpdate 抛 filesystem 错误）
        // 也保证 RemoveCallback 执行，杜绝 callback 泄漏与 owner 悬垂。
        ~DasProgressReporter();
        // 上报字段增量；未 Start / 已 Stop / 订阅全断开时为空操作。
        void Report(const UpdateProgress& progress);
        // 终态字段增量：updateProgress=文案（i18n progress.updateDone /
        // progress.updateFailed 模板 {0} 填 detail）+ downloadRate 空串 +
        // downloadElapsed 空串。后台更新 job 完成/失败时调用，前端经同一
        // SSE 字段通道收到终态（零新协议）。
        void ReportFinal(bool ok, const std::string& detail);

        // checkUpdate 三终局（后台检查 job 查完调用，D-05）：
        // - FoundUpdate：updateVersion（「cur → next」三元退化构造，空侧
        //   退化单值）+ releaseNote（info.release_note 原文，可为空串）+
        //   updateProgress（progress.updateFound 模板 {0}=目标版本）。
        // - UpToDate：updateVersion 空串 + updateProgress
        //   （progress.upToDate 模板 {0}=当前版本）。
        // - Failed：updateVersion 空串 + updateProgress
        //   （progress.checkFailed 模板 {0}=失败分类文案）。
        // 不推 downloadRate/downloadElapsed（检查链无下载指标）。缺译回退
        // 硬编码中文文案（照 ReportFinal 降级范式）。
        enum class CheckOutcomeKind
        {
            FoundUpdate,
            UpToDate,
            Failed
        };
        void ReportCheckOutcome(
            CheckOutcomeKind    kind,
            const PiUpdateInfo& info,
            const std::string&  current_version,
            const std::string&  failure_detail);

        // 字段属性 patch 推送（构造对象 value 字段增量的唯一入口）：patch 逐键
        // 过白名单，白名单外键丢弃该键 + i18n 报错（推送方是唯一校验位，传输
        // 侧哑管道不校验）；过滤后仍有键则组装 {key:value} 对象，经序列化往返
        // 转 IDasJson 上报（照 MakeJsonFieldValue 范式）。全键被丢 / 未 Start /
        // 已断开时为空操作。
        void ReportFieldPatch(
            std::string_view                                   field_name,
            std::vector<std::pair<std::string, yyjson::value>> patch);

        // 更新按钮态推送：单 entry 双字段（checkUpdate / applyUpdate 各携带
        // {disabled:bool} 对象 value）。job 终态在 Finish 之后调用（IsRunning
        // 已复位，推送值与再投影一致——按钮灰死 bug 的修复链）。
        void ReportUpdateButtonStates(bool check_disabled, bool apply_disabled);

        // 该 key 所有 SSE 订阅断开（OnAllSubscribersDisconnected 命中 own
        // key）。
        bool IsDisconnected() const { return disconnected_.load(); }

        // 注入外部翻译表（测试缝）。覆盖懒加载：传入非空 i18n 后
        // GetI18n 直接返回它，不再走 this_line_location/i18n 装载。
        // 传空清除注入并重置尝试标志，下次 GetI18n 重新懒加载。
        void SetI18n(DasPtr<Das::ExportInterface::IDasI18n> i18n)
        {
            i18n_ = std::move(i18n);
            i18n_load_attempted_ = (i18n_ != nullptr);
        }

    private:
        class DasProgressCallbackImpl;
        void OnSubscribersDisconnected();

        // 取翻译表：优先注入（SetI18n），否则首次调用懒加载
        // <plugin_dir>/i18n（boost::dll::this_line_location 推导）。
        // 目录缺失/装载失败永久返回 nullptr（不重试），降级硬编码文本。
        Das::ExportInterface::IDasI18n* GetI18n();
        // 把 UpdateProgress 投影成显示文本。i18n 就绪时走
        // Translate(key)+vformat 填 {N}；否则回退硬编码字面量。
        std::string BuildProgressText(const UpdateProgress& progress);

        std::string                                        key_;
        DasPtr<Das::ExportInterface::IDasProgressService>  service_;
        DasPtr<Das::ExportInterface::IDasProgressCallback> callback_;
        DasPtr<Das::ExportInterface::IDasI18n>             i18n_;
        std::atomic<bool>                                  disconnected_{false};
        bool                                               started_ = false;
        bool i18n_load_attempted_ = false;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

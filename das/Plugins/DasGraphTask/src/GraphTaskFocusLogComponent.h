#ifndef DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKFOCUSLOGCOMPONENT_H
#define DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKFOCUSLOGCOMPONENT_H

#include <das/DasGuidHolder.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasLogRecorder.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>

#include <cassert>
#include <cpp_yyjson.hpp>
#include <string_view>

namespace Das::Plugins::DasGraphTask
{
    class GraphTaskFocusLogComponent;
} // namespace Das::Plugins::DasGraphTask

// {609A9F19-CF3B-4546-B35E-E8B1C9BB9141}
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    GraphTaskFocusLogComponent,
    0x609a9f19,
    0xcf3b,
    0x4546,
    0xb3,
    0x5e,
    0xe8,
    0xb1,
    0xc9,
    0xbb,
    0x91,
    0x41)

namespace Das::Plugins::DasGraphTask
{

    constexpr std::string_view FOCUS_LOG_KIND = "das.graph.focus-log";

    // 组件身份 GUID：manifest 键/本常量/工厂分派三处共用同一字面量（无
    // 括号纯文本，测试钉值防漂移）。
    constexpr std::string_view FOCUS_LOG_COMPONENT_GUID_TEXT =
        "CFAF95AF-5ABC-422C-8CDB-0346F0526236";

    // focus-log 节点：message 输入口流入的上游文本写入 user.jsonl 焦点
    // 日志（UI 事件日志面板）。条目级别由 config 的 error/warning
    // checkbox 推导（error 优先 → warning → 默认 info）；写入链路任何失
    // 败（recorder resolve 失败/Record 失败）按 [focus-degraded] 前缀降
    // 级落调试日志，节点永不因此失败。无输出端口，Do 恒产有效空
    // port map。
    class GraphTaskFocusLogComponent final
        : public Das::PluginInterface::DasTaskComponentImplBase<
              GraphTaskFocusLogComponent>
    {
        // 借用非拥有：测试持 fixture recorder 保活注入；生产恒 nullptr，
        // Do 内经 C ABI 具名服务 resolve。
        Das::ExportInterface::IDasProcessLogRecorder* injected_recorder_ =
            nullptr;
        yyjson::value settings_;

    public:
        explicit GraphTaskFocusLogComponent(
            Das::ExportInterface::IDasProcessLogRecorder* injected_recorder =
                nullptr);

        // 级别推导（可直测）：error 勾 → ERROR；否则 warning 勾 →
        // WARNING；否则 INFO。
        static Das::ExportInterface::DasLogType DeriveEntryType(
            bool error_checked,
            bool warning_checked);

        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL ApplySettingsChange(
            Das::ExportInterface::IDasJson*  p_request_json,
            Das::ExportInterface::IDasJson** pp_out_result_json) override;

        DAS_IMPL Do(
            Das::PluginInterface::IDasStopToken*       stop_token,
            Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
            Das::ExportInterface::IDasPortMap** pp_out_port_map) override;

    private:
        // settings_ 的 bool 键读取：缺键/非 bool → false（缺省即语义）；
        // 保守 as_object()+contains+as_bool 形态防 caster 陷阱。
        bool FindSettingsBool(std::string_view key) const;
    };

} // namespace Das::Plugins::DasGraphTask

#endif // DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKFOCUSLOGCOMPONENT_H

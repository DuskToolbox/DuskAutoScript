#ifndef DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLTASKCOMPONENTS_H
#define DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLTASKCOMPONENTS_H

#include <cassert>

#include <cpp_yyjson.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// {523148A6-E5BD-4B32-B8C1-C9820214014C}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    DasFlowControlTaskComponent,
    0x523148a6,
    0xe5bd,
    0x4b32,
    0xb8,
    0xc1,
    0xc9,
    0x82,
    0x02,
    0x14,
    0x01,
    0x4c);

// {2C6DFCFA-F8B6-4272-8F28-1D31738BD0D4}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    DasFlowControlTaskComponentFactory,
    0x2c6dfcfa,
    0xf8b6,
    0x4272,
    0x8f,
    0x28,
    0x1d,
    0x31,
    0x73,
    0x8b,
    0xd0,
    0xd4);

DAS_NS_BEGIN

// 一次窗口枚举结果（通用 exe/窗口等待原语，不含任何业务语义）。
struct FlowControlWindowInfo
{
    void*       handle = nullptr;
    std::string class_name;
    std::string window_name;
};

// 进程启动缝：生产实现走 boost::process v2 spawn 并立即 detach（组件 Do
// 返回后子进程存活）；测试注入 lambda 驱动成功/失败与参数捕获，无需真进程。
using FlowControlProcessSpawner = std::function<bool(
    const std::string&              exe_path,
    const std::vector<std::string>& arguments,
    const std::string&              working_directory)>;

// 窗口枚举缝：生产实现 EnumWindows + GetClassNameW/GetWindowTextW；测试
// 注入 lambda 驱动 0/1/N 匹配序列，无需真窗口。
using FlowControlWindowEnumerator =
    std::function<std::vector<FlowControlWindowInfo>()>;

class DasFlowControlTaskComponent final
    : public PluginInterface::DasTaskComponentImplBase<
          DasFlowControlTaskComponent>
{
public:
    DasFlowControlTaskComponent(
        std::string_view                               kind,
        DasPtr<PluginInterface::IDasTaskComponentHost> host);

    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL ApplySettingsChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json) override;

    DAS_IMPL Do(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map) override;

    // 测试缝：注入进程启动/窗口枚举替身。传空 std::function 恢复生产实现
    // （生产实现于首次 DoLaunch 时惰性安装）。
    void SetProcessSpawnerForTest(FlowControlProcessSpawner spawner);
    void SetWindowEnumeratorForTest(FlowControlWindowEnumerator enumerator);

private:
    DasResult DoRepositoryInvoke(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Pure signal routing: read `condition`, emit the `true`/`false` signal.
    // The runtime gates the downstream branch on the emitted signal (DAS-60
    // Stage 4).
    DasResult DoBranch(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Counted loop head: maintains `loop_index_` across Do() calls. Each call
    // writes the current `index` output and emits `continue`, or emits `break`
    // when the range is exhausted. The body is a separate main-graph node
    // re-activated by the runtime via the continue signal + back-edge.
    DasResult DoFor(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Condition loop head: reads `condition` each call and emits
    // `continue`/`break`. No internal state.
    DasResult DoWhile(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // φ-join: gated by in_true/in_false signals; copies whichever of
    // value_true/value_false the taken branch produced into `result`.
    DasResult DoMerge(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Cancellable timed wait: blocks for `delay_ms` (config), then emits
    // "next". stop_token is a poll-only interface with no callback, so the wait
    // is sliced
    // (~50ms) on a condition variable and re-polls stop each slice; a stop
    // during the wait returns "cancelled" promptly. delay_ms <= 0 skips the
    // wait.
    DasResult DoDelay(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // 启动进程原语：读 config（exePath/args/workingDirectory/
    // waitWindowClassRegex/waitWindowWindowRegex/waitWindowTimeoutMs），
    // spawn（成功立即 detach）后可选等待目标窗口出现（轮询窗口枚举缝，
    // 小时间片可取消；0/1/N 匹配语义）。成功发 signal_out；失败带结构化
    // diagnostic（launch-exe-path-missing / launch-spawn-failed /
    // launch-window-timeout / launch-window-ambiguous，消息经 i18n）。
    DasResult DoLaunch(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    std::string                                    kind_;
    yyjson::value                                  settings_;
    DasPtr<PluginInterface::IDasTaskComponentHost> host_;

    // for-loop counter state. A fresh component instance is created per graph
    // execution (GraphRuntime::Configure), so this naturally resets per run;
    // it persists across the Do() calls of a single loop.
    int64_t loop_index_ = 0;
    bool    loop_started_ = false;

    // 注入缝：空 = 用生产实现（首次 DoLaunch 惰性安装）。
    FlowControlProcessSpawner   process_spawner_;
    FlowControlWindowEnumerator window_enumerator_;
};

class DasFlowControlTaskComponentFactory final
    : public PluginInterface::DasTaskComponentFactoryImplBase<
          DasFlowControlTaskComponentFactory>
{
public:
    explicit DasFlowControlTaskComponentFactory(
        PluginInterface::IDasTaskComponentHost* p_host);

    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL CreateComponent(
        const DasGuid&                       component_guid,
        PluginInterface::IDasTaskComponent** pp_out_component) override;

private:
    DasPtr<PluginInterface::IDasTaskComponentHost> host_;
};

DAS_NS_END

#endif // DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLTASKCOMPONENTS_H

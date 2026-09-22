#ifndef DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLTASKCOMPONENTS_H
#define DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLTASKCOMPONENTS_H

#include <cassert>
#include <cstdint>

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

// 单次启动尝试的三态结果：Spawned=已启动；Failed=启动失败；ElevationDeclined
// =目标 manifest 要求提权且回退启动的 UAC 请求被用户拒绝（专用失败码出口）。
enum class FlowControlSpawnStatus
{
    Spawned,
    Failed,
    ElevationDeclined
};

// 进程启动结果。bool 转换构造保留存量注入缝的返回语义（true=Spawned /
// false=Failed），既有测试 lambda 源码零改动继续编译。
struct FlowControlSpawnResult
{
    FlowControlSpawnStatus status = FlowControlSpawnStatus::Failed;

    FlowControlSpawnResult() = default;

    FlowControlSpawnResult(bool spawned)
        : status(
              spawned ? FlowControlSpawnStatus::Spawned
                      : FlowControlSpawnStatus::Failed)
    {
    }

    FlowControlSpawnResult(FlowControlSpawnStatus spawn_status)
        : status(spawn_status)
    {
    }
};

// 进程启动缝：生产实现在主启动（boost::process v2 spawn + 立即 detach，
// 组件 Do 返回后子进程存活）遇目标要求提权（Win32 740）时回退
// ShellExecuteExW 提权启动（UAC 由 OS 协商弹窗，host 自身不提权）；
// ElevationDeclined 对应 UAC 被拒。capture_output=true 时主启动接捕获管道
// 逐行落盘（提权回退路径无捕获）。测试注入 lambda 驱动三态与参数捕获，
// 无需真进程。
using FlowControlProcessSpawner = std::function<FlowControlSpawnResult(
    const std::string&              exe_path,
    const std::vector<std::string>& arguments,
    const std::string&              working_directory,
    bool                            capture_output)>;

// 提权回退启动尝试的三态结果。
enum class FlowControlElevatedLaunchStatus
{
    Spawned,
    Declined,
    Failed
};

// 提权回退启动结果：Spawned 携带子进程 pid（OS 未给进程句柄时 pid=0）。
struct FlowControlElevatedLaunchOutcome
{
    FlowControlElevatedLaunchStatus status =
        FlowControlElevatedLaunchStatus::Failed;
    std::uint32_t process_id = 0;
};

// 提权回退启动缝：生产实现 ShellExecuteExW（普通 open verb，目标 manifest
// requireAdministrator 的 exe 由 OS 协商 UAC 弹窗）。测试注入桩驱动三态，
// 无需真机 UAC。
using FlowControlElevatedLauncher =
    std::function<FlowControlElevatedLaunchOutcome(
        const std::string&              exe_path,
        const std::vector<std::string>& arguments,
        const std::string&              working_directory)>;

// 测试注入 hook：传空 std::function 恢复生产实现。
void SetElevatedLauncherForTest(FlowControlElevatedLauncher launcher);

// 主启动尝试体：成功返回子进程 pid，失败抛异常（boost::process v2 构造
// 语义原样）；是否回退提权由编排函数按异常分类决定。
using FlowControlPrimarySpawnBody = std::function<std::uint32_t(
    const std::string&              exe_path,
    const std::vector<std::string>& arguments,
    const std::string&              working_directory,
    bool                            capture_output)>;

// 启动编排：主启动体成功即 Spawned；主启动抛 Win32 740
// （ERROR_ELEVATION_REQUIRED）时经提权启动缝回退（三态映射 Spawned/
// ElevationDeclined/Failed）；其余异常一律 Failed。双注入缝使 740 分类/
// 拒绝/成功/非 740 跳过全链路可直测。
FlowControlSpawnResult SpawnProcessWithElevationFallback(
    const FlowControlPrimarySpawnBody& primary_spawn_body,
    const FlowControlElevatedLauncher& elevated_launcher,
    const std::string&                 exe_path,
    const std::vector<std::string>&    arguments,
    const std::string&                 working_directory,
    bool                               capture_output);

// 窗口枚举缝：生产实现 EnumWindows + GetClassNameW/GetWindowTextW；测试
// 注入 lambda 驱动 0/1/N 匹配序列，无需真窗口。
using FlowControlWindowEnumerator =
    std::function<std::vector<FlowControlWindowInfo>()>;

class DasFlowControlTaskComponent final
    : public PluginInterface::DasTaskComponentImplBase<
          DasFlowControlTaskComponent>
{
public:
    DasFlowControlTaskComponent(std::string_view kind);

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
    // Pure signal routing: read `condition`, emit the `true`/`false` signal.
    // The runtime gates the downstream branch on the emitted signal (DAS-60
    // Stage 4).
    DasResult DoBranch(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Counted loop head: the counter lives in the frame (loopCarried `index`
    // port pair — the compiler synthesizes the self-loop value route), so the
    // component itself is stateless across Do() calls and an outer-loop
    // restart resets the count naturally. Each call writes the current
    // `index` output and emits `continue`, or emits `break` when the range is
    // exhausted. The body is a separate main-graph node re-activated by the
    // runtime via the continue signal + back-edge.
    DasResult DoFor(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Condition loop head: reads `condition` each call and emits
    // `continue`/`break`; the first iteration falls back to the
    // `first_condition` port (default true) while the loop-carried condition
    // has produced no value yet. No internal state.
    DasResult DoWhile(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Double comparison: reads left/right (falling back to config panel
    // defaults on unconnected ports), applies the configured operator and
    // writes the bool `result`. Operator outside < <= > >= == != is a
    // fail-closed error (never silently coerced).
    DasResult DoCompareDouble(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Int64 comparison: reads left/right (falling back to config panel
    // defaults on unconnected ports), applies the configured operator and
    // writes the bool `result`. Operator outside < <= > >= == != is a
    // fail-closed error (never silently coerced).
    DasResult DoCompareInt64(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // String comparison: reads left/right (falling back to config panel
    // defaults on unconnected ports) and writes the bool `result`. The
    // operator vocabulary is closed at equality only (== !=) — ordering
    // operators have no defined string semantics here and return the
    // fail-closed operator error instead of silently coercing.
    DasResult DoCompareString(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Double arithmetic: reads left/right (falling back to config panel
    // defaults on unconnected ports), applies the configured operator and
    // writes `result`. Operator outside + - * / is a fail-closed error;
    // division by zero is rejected, not silently mapped to inf.
    DasResult DoArithmeticNumber(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Typed constants: write the config `value` key (falling back to the
    // type's zero value when the key is absent) to the `value` output port
    // on every execution — no compile-time constant folding special case
    // (a constant producer without a signal in-edge is auto-evaluated by
    // the closure rule instead).
    DasResult DoConstantBool(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // See DoConstantBool.
    DasResult DoConstantInt64(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // See DoConstantBool.
    DasResult DoConstantDouble(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // See DoConstantBool.
    DasResult DoConstantString(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // String concatenation: reads left/right (falling back to config panel
    // defaults on unconnected ports) and writes result = left + right.
    DasResult DoConcatString(
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
    // 小时间片可取消；0/1/N 匹配语义）。成功后规范口 signal_out 的发射
    // 归 GraphRuntime 兜底（组件只发具名控制口）；失败全部属
    // 「无法完成启动动作」= 执行失败，返回 DAS_E_FLOW_LAUNCH_* 专用错误码
    // （GraphRuntime 失败接管路径终止图），同时尽力构建 diagnostic 载荷
    // （launch-exe-path-missing / launch-spawn-failed /
    // launch-window-timeout / launch-window-ambiguous，消息经 i18n）。
    DasResult DoLaunch(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    std::string   kind_;
    yyjson::value settings_;

    // 注入缝：空 = 用生产实现（首次 DoLaunch 惰性安装）。
    FlowControlProcessSpawner   process_spawner_;
    FlowControlWindowEnumerator window_enumerator_;
};

class DasFlowControlTaskComponentFactory final
    : public PluginInterface::DasTaskComponentFactoryImplBase<
          DasFlowControlTaskComponentFactory>
{
public:
    DasFlowControlTaskComponentFactory() = default;

    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL CreateComponent(
        const DasGuid&                       component_guid,
        PluginInterface::IDasTaskComponent** pp_out_component) override;
};

DAS_NS_END

#endif // DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLTASKCOMPONENTS_H

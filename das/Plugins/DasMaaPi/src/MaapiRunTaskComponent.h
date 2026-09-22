#ifndef DAS_PLUGINS_DASMAAPI_MAAPIRUNTASKCOMPONENT_H
#define DAS_PLUGINS_DASMAAPI_MAAPIRUNTASKCOMPONENT_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

#include <cassert>

#include <cpp_yyjson.hpp>

#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiRunTaskComponent,
    0x69f20008,
    0x0000,
    0x4000,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x01);

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiRunTaskComponentFactory,
    0x69f20009,
    0x0000,
    0x4000,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x01);

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    // manifest（DasMaaPi.json）声明的执行组件：config 声明 repositoryRef
    // （type=taskRepositoryRef，值 = 纯 entryId 整数；供编译期注入链识别
    // 节点绑定的条目，运行期不参与供数）与 runtimeInputs（object）两键、
    // 输入口 executionInput（object）、输出口
    // completedTasks/stopped/diagnostics、信号 succeeded/failed/cancelled。
    // Do 的 envelope 唯一来源 = 输入口 executionInput 喂线（父图编译期
    // 注入链送达）；端口缺失遇缺即错返回
    // DAS_E_MAAPI_EXECUTION_INPUT_MISSING，运行期零自解析回退、零主进程
    // 服务查询。
    class MaapiRunTaskComponent final
        : public PluginInterface::DasTaskComponentImplBase<
              MaapiRunTaskComponent>
    {
    public:
        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL ApplySettingsChange(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;

        DAS_IMPL Do(
            PluginInterface::IDasStopToken*       stop_token,
            ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
            ExportInterface::IDasPortMap**        pp_out_port_map) override;

        // fake clock seam：nextTime 输出口（D-B5 共享 schedule brain）的计算
        // 时钟。生产默认 std::time(nullptr)；单测注入固定时钟（照 MaapiTask::
        // SetClockForTest 同范式）。
        using ClockFn = std::function<std::time_t()>;
        void SetClockForTest(ClockFn fn) { now_ = std::move(fn); }

    private:
        // settings 存储锁：ApplySettingsChange（Configure 阶段投递）与 Do
        // （执行）可能来自不同线程。
        mutable std::mutex settings_mutex_;
        // runtimeInputs 存储值（对象级运行时输入覆盖）；null = 未设置。
        // repositoryRef 不存储：编译期注入链直接读图文档节点的 settings，
        // 组件运行期不消费条目 id。
        yyjson::value runtime_inputs_;
        // nextTime 口的计算时钟（D-B5）。
        ClockFn now_ = [] { return std::time(nullptr); };
    };

    class MaapiRunTaskComponentFactory final
        : public PluginInterface::DasTaskComponentFactoryImplBase<
              MaapiRunTaskComponentFactory>
    {
    public:
        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL CreateComponent(
            const DasGuid&                       component_guid,
            PluginInterface::IDasTaskComponent** pp_out_component) override;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPIRUNTASKCOMPONENT_H

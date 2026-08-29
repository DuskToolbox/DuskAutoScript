#ifndef DAS_PLUGINS_DASMAAPI_MAAPIRUNTASKCOMPONENT_H
#define DAS_PLUGINS_DASMAAPI_MAAPIRUNTASKCOMPONENT_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

#include <cassert>

#include <cpp_yyjson.hpp>

#include <cstdint>
#include <mutex>

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
    // （type=taskRepositoryRef，值 = 纯 entryId 整数）与 runtimeInputs
    // （object）两键、输入口 executionInput（object）、输出口
    // completedTasks/stopped/diagnostics、信号 succeeded/failed/cancelled。
    // Do 消费 settings 的 repositoryRef/runtimeInputs 两键：executionInput
    // 喂线时行为与纯消费者一致；executionInput 缺失且 repositoryRef 有效时
    // 自行解析仓库条目并执行（envelope 经主进程调度服务编译产出）。
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

    private:
        // settings 存储锁：ApplySettingsChange（Configure 阶段投递）与 Do
        // （执行）可能来自不同线程。
        mutable std::mutex settings_mutex_;
        // repositoryRef 存储值（纯 entryId 整数）；0 = 未设置。
        int64_t repository_entry_id_ = 0;
        // runtimeInputs 存储值（对象级运行时输入覆盖）；null = 未设置。
        yyjson::value runtime_inputs_;
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

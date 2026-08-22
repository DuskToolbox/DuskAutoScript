#ifndef DAS_PLUGINS_DASMAAPI_MAAPIEXECUTIONKERNEL_H
#define DAS_PLUGINS_DASMAAPI_MAAPIEXECUTIONKERNEL_H

#include <cassert>

#include <cpp_yyjson.hpp>
#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/_autogen/idl/abi/IDasTask.h>

#include <map>
#include <string>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    // 两个 Do ABI 面（MaapiTask::Do / MaapiRunTaskComponent::Do）的共享执行
    // 内核。流水线：ParseExecutionEnvelope（失败即返回）→ MergePortMapInputs
    // （port_map 与 inputs 任一为空即空操作；port_map 来源在运行期传参通道
    // 接线前保持留空，签名先行保留）→ stop token 前置检查 → MaaRuntime::Run
    // → LogRunResult。schedule 语义（daily slot 标记）留在 Task 侧，port
    // I/O（输入消费/输出回填）留在 Component 侧——接口职责差异不进内核。
    // boundary 经参数注入：生产两侧传 MaaApiBoundaryForRuntime()，测试传
    // FakeMaaApiBoundary。
    DasResult ExecuteEnvelope(
        const yyjson::value&                       envelope_json,
        IMaaApiBoundary&                           boundary,
        PluginInterface::IDasStopToken*            stop_token,
        const std::map<std::string, std::string>&  port_map,
        const std::map<std::string, yyjson::value> inputs,
        MaaRuntimeResult&                          out_result);
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPIEXECUTIONKERNEL_H

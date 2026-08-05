#pragma once

#include "AgentProcessRunner.h"

#include <das/Plugins/DasMaaPi/AgentRuntimeDto.h>
#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>

#include <functional>
#include <memory>
#include <string_view>

namespace Das::Plugins::DasMaaPi::AgentRuntime
{
    struct AgentRuntimeMaaContext
    {
        MaaResourceHandle   resource = INVALID_MAA_RESOURCE_HANDLE;
        MaaControllerHandle controller = INVALID_MAA_CONTROLLER_HANDLE;
        MaaTaskerHandle     tasker = INVALID_MAA_TASKER_HANDLE;
    };

    // 逐行分发子进程 stdout/stderr（is_stderr 区分两流）。caller 在 MaaRuntime
    // 注入闭包：捕获 recorder + agent_id（service 在 Start 内 per-agent 绑
    // agent_id 后传入 AgentProcessLaunchRequest.on_output，故 caller 这里收
    // 到的 agent_id 已是当前行所属 agent）。service 自身不引用 recorder，
    // 维持 layer 分离（recorder 闭包由 MaaRuntime 构造，service 透传）。
    using AgentOutputCallback = std::function<
        void(std::string_view agent_id, std::string_view line, bool is_stderr)>;

    class AgentRuntimeService final
    {
    public:
        AgentRuntimeService(
            IMaaApiBoundary&     boundary,
            IAgentProcessRunner& runner);
        ~AgentRuntimeService();

        AgentRuntimeService(const AgentRuntimeService&) = delete;
        AgentRuntimeService& operator=(const AgentRuntimeService&) = delete;

        AgentRuntimeResultDto Start(
            const AgentRuntimeRequestDto& request,
            const AgentRuntimeMaaContext& context,
            const AgentOutputCallback&    on_output = {});
        AgentRuntimeResultDto Stop(
            std::string_view              session_id,
            const AgentRuntimeOptionsDto& options = {});
        AgentRuntimeResultDto Status(std::string_view session_id);

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace Das::Plugins::DasMaaPi::AgentRuntime

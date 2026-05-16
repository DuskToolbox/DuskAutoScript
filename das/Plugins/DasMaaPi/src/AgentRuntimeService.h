#pragma once

#include "AgentProcessRunner.h"

#include <das/Plugins/DasMaaPi/AgentRuntimeDto.h>
#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>

#include <memory>
#include <string_view>

namespace Das::Plugins::DasMaaPi::AgentRuntime
{
    struct AgentRuntimeMaaContext
    {
        MaaResourceHandle   resource = kInvalidMaaResourceHandle;
        MaaControllerHandle controller = kInvalidMaaControllerHandle;
        MaaTaskerHandle     tasker = kInvalidMaaTaskerHandle;
    };

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
            const AgentRuntimeMaaContext& context);
        AgentRuntimeResultDto Stop(
            std::string_view              session_id,
            const AgentRuntimeOptionsDto& options = {});
        AgentRuntimeResultDto Status(std::string_view session_id);

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace Das::Plugins::DasMaaPi::AgentRuntime

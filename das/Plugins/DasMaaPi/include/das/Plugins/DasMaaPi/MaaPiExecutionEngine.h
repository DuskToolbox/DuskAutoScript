#pragma once

#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/_autogen/idl/abi/IDasTask.h>

#include <cpp_yyjson.hpp>

#include <map>
#include <string>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    struct EngineInput
    {
        std::string                          pi_path;
        std::string                          task_name;
        yyjson::value                        options;
        std::map<std::string, std::string>   port_map;
        std::map<std::string, yyjson::value> inputs;
    };

    struct EngineOutput
    {
        DasResult                            das_result = DAS_S_OK;
        std::string                          error_message;
        std::vector<std::string>             completed_tasks;
        std::map<std::string, yyjson::value> outputs;
        std::vector<MaaRuntimeDiagnostic>    diagnostics;
    };

    class MaaPiExecutionEngine
    {
    public:
        EngineOutput Execute(
            const EngineInput&              input,
            IMaaApiBoundary&                boundary,
            PluginInterface::IDasStopToken* stop_token);
    };

} // namespace Das::Plugins::DasMaaPi

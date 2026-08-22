#include "MaapiExecutionKernel.h"

#include <das/DasApi.h>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    DasResult ExecuteEnvelope(
        const yyjson::value&                       envelope_json,
        IMaaApiBoundary&                           boundary,
        PluginInterface::IDasStopToken*            stop_token,
        const std::map<std::string, std::string>&  port_map,
        const std::map<std::string, yyjson::value> inputs,
        MaaRuntimeResult&                          out_result)
    {
        (void)envelope_json;
        (void)boundary;
        (void)stop_token;
        (void)port_map;
        (void)inputs;
        out_result = MaaRuntimeResult{};
        return DAS_E_NO_IMPLEMENTATION;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END

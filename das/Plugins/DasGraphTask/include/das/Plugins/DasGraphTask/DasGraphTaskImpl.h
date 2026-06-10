#ifndef DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKIMPL_H
#define DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKIMPL_H

#include <das/DasApi.h>
#include <das/DasGuidHolder.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>

#include <string>

namespace Das::Plugins::DasGraphTask
{
    class DasGraphTaskImpl;
} // namespace Das::Plugins::DasGraphTask

// IID for DasGraphTaskImpl — required by IDasTypeInfo::GetGuid and DasIidOf
// lookups from other translation units (e.g., DasGraphTaskPluginPackage.cpp).
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    DasGraphTaskImpl,
    0xF4A2C9D1,
    0x7B3E,
    0x4D8A,
    0xA1,
    0x5F,
    0xC2,
    0xE8,
    0x6B,
    0x4D,
    0x91,
    0xA3)

namespace Das::Plugins::DasGraphTask
{

    // Thin adapter: IDasTaskComponent -> GraphRuntime execution.
    // Delegates to IDasGraphRuntime::Execute() via the public DasCore API.
    class DasGraphTaskImpl final
        : public Das::PluginInterface::DasTaskComponentImplBase<
              DasGraphTaskImpl>
    {
        std::string                                         last_error_;
        DasPtr<Das::PluginInterface::IDasTaskComponentHost> host_;

    public:
        explicit DasGraphTaskImpl(
            Das::PluginInterface::IDasTaskComponentHost* p_host);

        // --- IDasTaskComponent interface ---
        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL ApplySettingsChange(
            Das::ExportInterface::IDasJson*  p_request_json,
            Das::ExportInterface::IDasJson** pp_out_result_json) override;

        DAS_IMPL Do(
            Das::PluginInterface::IDasStopToken*       stop_token,
            Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
            Das::ExportInterface::IDasPortMap** pp_out_port_map) override;
    };

} // namespace Das::Plugins::DasGraphTask

#endif // DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKIMPL_H

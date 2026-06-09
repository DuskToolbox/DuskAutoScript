#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIMEFACTORY_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIMEFACTORY_H

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasGraphRuntime.Implements.hpp>

#include <string>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// Internal C++ implementation of IDasGraphRuntime COM interface.
// Wraps the engine-level GraphRuntime class behind a COM-compatible facade.
// Inherits from autogen ImplBase for automatic AddRef/Release/QueryInterface.
class GraphRuntimeImpl final
    : public Das::ExportInterface::DasGraphRuntimeImplBase<GraphRuntimeImpl>
{
    GraphRuntime engine_;
    std::string  last_error_;

public:
    GraphRuntimeImpl() = default;

    // --- IDasGraphRuntime interface ---
    DAS_IMPL Load(IDasReadOnlyString* p_compiled_artifact_json);

    DAS_IMPL Configure(IDasReadOnlyString* p_node_snapshots_json);

    DAS_IMPL Run(Das::PluginInterface::IDasStopToken* p_stop_token);

    DAS_IMPL GetErrorMessage(IDasReadOnlyString** pp_out_error_message);
};

DAS_CORE_GRAPHRUNTIME_NS_END

// C ABI factory — primary declaration is auto-generated in
// DasCoreApi.generated.h.  This forward declaration ensures C++ code can
// call it before CMake reconfigure.
DAS_C_API DasResult
CreateGraphRuntime(Das::ExportInterface::IDasGraphRuntime** pp_out_runtime);

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHRUNTIMEFACTORY_H

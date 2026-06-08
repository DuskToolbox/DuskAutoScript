#ifndef DAS_CORE_GRAPHRUNTIME_DOADAPTER_H
#define DAS_CORE_GRAPHRUNTIME_DOADAPTER_H

#include <string>
#include <vector>

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/PortFrame.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// ---------------------------------------------------------------------------
// DoAdapter — PortFrame ↔ IDasPortMap / IDasReadOnlyPortMap conversion layer
//
// Bridges the internal PortFrame (keyed by DasGuid+port_id) with the public
// v42 ABI contract  Do(stop_token, IDasReadOnlyPortMap* in, IDasPortMap** out).
//
// BuildInputPortMap:  reads upstream PortFrame entries via the binding plan,
//                     populates a fresh IDasPortMap (exposed as read-only).
//
// ExtractOutputPortMap: reads the IDasPortMap returned by Do(), writes every
//                       entry back into the PortFrame under (node_id, port_id).
// ---------------------------------------------------------------------------

/// Read upstream PortFrame entries described by @p bindings and populate a
/// fresh IDasPortMap.  Each binding's source_node_id + source_port_id is
/// looked up in @p frame; the value is inserted with the target_port_id as
/// key (v44: string-keyed, never by index).
///
/// @param frame     Current execution snapshot.
/// @param bindings  Data-flow edges filtered to the target node's inputs.
/// @param out_map   Receives a newly allocated IDasPortMap (caller owns).
/// @return DAS_S_OK on success.
DasResult BuildInputPortMap(
    const PortFrame&                        frame,
    const std::vector<Dto::PortBindingDto>& bindings,
    Das::ExportInterface::IDasPortMap**     out_map);

/// Read all entries from @p output_map (returned by Do()) and write them
/// into @p frame under the given @p node_id.
///
/// @param output_map  The IDasPortMap produced by the node's Do() call.
/// @param node_id     GUID of the node that produced this output.
/// @param frame       The execution frame to update.
/// @return DAS_S_OK on success.
DasResult ExtractOutputPortMap(
    Das::ExportInterface::IDasPortMap* output_map,
    DasGuid                            node_id,
    PortFrame&                         frame);

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_DOADAPTER_H

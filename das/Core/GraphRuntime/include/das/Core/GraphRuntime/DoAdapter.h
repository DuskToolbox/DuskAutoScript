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
// Bridges the internal PortFrame (keyed by node_id string + port_id) with the
// public v42 ABI contract  Do(stop_token, IDasReadOnlyPortMap* in,
// IDasPortMap** out).
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
/// A broadcast (graph-level input) binding first consults @p graph_inputs:
/// when it holds the binding's target_port_id as a typed entry, that value
/// is transferred verbatim (the declared default is not materialised) —
/// the value precedence is call-site input > port default > component
/// default.
///
/// @param frame        Current execution snapshot.
/// @param bindings     Data-flow edges filtered to the target node's inputs.
/// @param out_map      Receives a newly allocated IDasPortMap (caller owns).
/// @param graph_inputs Typed graph-level inputs keyed by boundary port id;
///                     null keeps the default-only behaviour.
/// @return DAS_S_OK on success.
DasResult BuildInputPortMap(
    const PortFrame&                           frame,
    const std::vector<Dto::PortBindingDto>&    bindings,
    Das::ExportInterface::IDasPortMap**        out_map,
    Das::ExportInterface::IDasReadOnlyPortMap* graph_inputs = nullptr);

/// Read all entries from @p output_map (returned by Do()) and write them
/// into @p frame under the given @p node_id.
///
/// @param output_map  The IDasPortMap produced by the node's Do() call.
/// @param node_id     String node id of the node that produced this output.
/// @param frame       The execution frame to update.
/// @return DAS_S_OK on success.
DasResult ExtractOutputPortMap(
    Das::ExportInterface::IDasPortMap* output_map,
    const std::string&                 node_id,
    PortFrame&                         frame);

/// Write a frame PortValue into @p map under @p port_id, dispatching on the
/// variant kind. Object references (image/base/component) are written as
/// references — the JSON-serialisable subset only skips null/empty values
/// the way the JSON export channel does. Exposed for the typed boundary
/// export next to GraphRuntime's execution tail.
///
/// @param pv       Frame value to write.
/// @param port_id  Destination key.
/// @param map      Destination PortMap.
/// @return DAS_S_OK on success (skipped values also return DAS_S_OK).
DasResult PortValueToPortMap(
    const PortValue&                   pv,
    const std::string&                 port_id,
    Das::ExportInterface::IDasPortMap* map);

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_DOADAPTER_H

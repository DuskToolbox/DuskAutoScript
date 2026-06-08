#ifndef DAS_CORE_GRAPHRUNTIME_LEGACYJSONADAPTER_H
#define DAS_CORE_GRAPHRUNTIME_LEGACYJSONADAPTER_H

#include <das/Core/GraphRuntime/Config.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasReadOnlyPortMap.h>
#include <das/_autogen/idl/abi/IDasTask.h>

#include <functional>
#include <string>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// ---------------------------------------------------------------------------
// JSON ↔ PortMap conversion utilities
//
// ConvertJsonToPortMap:  parse a JSON string, populate an IDasPortMap.
//                        Each top-level key becomes a port_id.
//
// ConvertPortMapToJson:  serialize an IDasPortMap into a JSON string.
//                        Typed values map to JSON equivalents.
//
// MapDasResultToStatus:  map DasResult → "ok"/"cancelled"/"error".
// ---------------------------------------------------------------------------

/// Parse a JSON string and populate a PortMap.
///
/// Each top-level key in the JSON object becomes a port_id.
/// Supported mappings:
///   int    → SetInt      | real  → SetFloat
///   string → SetString   | bool  → SetBool
///   null   → skipped (no SetNull API)
///   object/array → serialized to string via SetString (GetJson = GetString)
///
/// @param input_json           JSON string to parse.
/// @param p_portmap            Target PortMap (must not be null).
/// @param p_out_error_message  Optional diagnostic on parse failure.
/// @return DAS_S_OK on success, DAS_E_INVALID_POINTER or DAS_E_INVALID_ARG on
///         failure.
DasResult ConvertJsonToPortMap(
    const std::string&                    input_json,
    Das::ExportInterface::IDasPortMap*    p_portmap,
    std::string*                          p_out_error_message = nullptr);

/// Serialize a PortMap into a JSON string.
///
/// IMAGE entries produce {"__das_image_ref__": {…}} placeholders.
/// JSON entries (stored as strings) are parsed and inlined as nested objects.
///
/// @param p_portmap        Source PortMap (must not be null).
/// @param out_result_json  Receives the serialized JSON string.
/// @return DAS_S_OK on success.
DasResult ConvertPortMapToJson(
    Das::ExportInterface::IDasPortMap* p_portmap,
    std::string&                       out_result_json);

/// Map DasResult to a human-readable status string for legacy consumers.
///
///   DAS_S_OK     → "ok"
///   DAS_E_TIMEOUT → "cancelled"
///   all others   → "error"
const char* MapDasResultToStatus(DasResult result);

// ---------------------------------------------------------------------------
// LegacyJsonTaskComponentAdapter
//
// Wraps a legacy JSON-based component (represented as a callback) and exposes
// a v42 PortMap-based Do() interface.  The adapter:
//   1. Converts input PortMap → JSON string
//   2. Calls the legacy Do callback with the JSON string
//   3. Converts result JSON string → output PortMap
//   4. Injects __status__ into the output PortMap
// ---------------------------------------------------------------------------

/// Legacy component callback type.
/// Receives input JSON string, produces output JSON string.
using LegacyDoFn = std::function<DasResult(
    const std::string& input_json,
    std::string&       out_result_json)>;

/// Adapter that wraps a legacy JSON-based component and exposes a v42
/// PortMap-based Do() interface.
class LegacyJsonTaskComponentAdapter
{
public:
    /// Construct an adapter wrapping the given legacy Do callback.
    explicit LegacyJsonTaskComponentAdapter(LegacyDoFn legacy_do);

    ~LegacyJsonTaskComponentAdapter();

    // Non-copyable.
    LegacyJsonTaskComponentAdapter(const LegacyJsonTaskComponentAdapter&) = delete;
    LegacyJsonTaskComponentAdapter& operator=(const LegacyJsonTaskComponentAdapter&) =
        delete;

    /// v42-compatible Do().
    ///
    /// Converts input PortMap to JSON, calls legacy Do, converts result JSON
    /// back to PortMap, and injects __status__.
    ///
    /// @param p_stop_token   Stop token (currently unused by callback).
    /// @param p_input        Input PortMap (read-only).
    /// @param pp_out_portmap Receives the newly allocated output PortMap.
    /// @return DAS_S_OK on success.
    DasResult Do(
        Das::PluginInterface::IDasStopToken*     p_stop_token,
        Das::ExportInterface::IDasReadOnlyPortMap* p_input,
        Das::ExportInterface::IDasPortMap**        pp_out_portmap);

private:
    LegacyDoFn legacy_do_;
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_LEGACYJSONADAPTER_H

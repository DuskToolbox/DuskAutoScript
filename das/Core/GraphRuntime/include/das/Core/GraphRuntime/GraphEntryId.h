#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHENTRYID_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHENTRYID_H

#include <das/Core/GraphRuntime/Config.h>
#include <cstdint>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

/// Type alias for graph entry identifiers.
/// Maps to the existing repository entry_id type (int64_t).
/// No new ID space — purely semantic clarity at the graph orchestration layer.
/// Per v34: {graphEntryId} = entry_id of a GraphTask entry.
using GraphEntryId = int64_t;

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHENTRYID_H

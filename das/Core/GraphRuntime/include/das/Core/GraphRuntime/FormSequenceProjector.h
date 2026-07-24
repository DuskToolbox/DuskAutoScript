#ifndef DAS_CORE_GRAPHRUNTIME_FORMSEQUENCEPROJECTOR_H
#define DAS_CORE_GRAPHRUNTIME_FORMSEQUENCEPROJECTOR_H

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphDocument.h>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// ---------------------------------------------------------------------------
// formSequence authoring model (DAS-77)
// ---------------------------------------------------------------------------
//
// A formSequence is the *authoring* representation of a linear task chain: an
// ordered list of items the user edits as a flat sequence. It is projected
// (see FormSequenceProjector::Project) into a GraphDocumentDto — the runtime
// representation — where every item becomes a node and consecutive items are
// joined by a linear signal edge. The runtime then compiles and executes that
// graph with the existing signal-gated engine (DAS-60).
//
// This mirrors the existing execution-side split (DasGraphTask thin adapter →
// Core GraphRuntime): the projection + compile capability lives entirely in
// Core; plugins only marshal JSON across the boundary.
// ---------------------------------------------------------------------------

namespace Dto
{
    /// One step in a linear formSequence. Carries the same target shape as a
    /// graph node (componentRef | entryRef) plus a free-form settings blob.
    struct FormSequenceItemDto
    {
        std::string        item_id;
        GraphNodeTargetDto target;
        yyjson::value      settings;
    };

    /// The formSequence authoring document — an ordered list of items plus the
    /// identity metadata shared with GraphDocumentDto.
    struct FormSequenceDto
    {
        std::string                      document_id;
        int32_t                          version = 0;
        std::string                      fingerprint;
        std::vector<FormSequenceItemDto> items;
    };
} // namespace Dto

// ---------------------------------------------------------------------------
// formSequence change requests (authoring-only, mirror GraphAuthoringChange)
// ---------------------------------------------------------------------------

/// Append an item to the end of the sequence.
struct AppendSequenceItemChange
{
    Dto::FormSequenceItemDto item;
};

/// Insert an item before the given zero-based index (index == size appends).
struct InsertSequenceItemChange
{
    std::size_t              index;
    Dto::FormSequenceItemDto item;
};

/// Move the item at `from` so that it ends up at `to` (both zero-based,
/// interpreted in the pre-move ordering).
struct MoveSequenceItemChange
{
    std::size_t from;
    std::size_t to;
};

/// Remove the item with the given item_id.
struct RemoveSequenceItemChange
{
    std::string item_id;
};

/// Replace the settings blob on an existing item.
struct SetSequenceItemValueChange
{
    std::string   item_id;
    yyjson::value settings;
};

/// Tag-dispatched union of all formSequence change types.
struct FormSequenceChange
{
    std::variant<
        AppendSequenceItemChange,
        InsertSequenceItemChange,
        MoveSequenceItemChange,
        RemoveSequenceItemChange,
        SetSequenceItemValueChange>
        payload;
};

// ---------------------------------------------------------------------------
// Authoring result
// ---------------------------------------------------------------------------

enum class FormSequenceErrorKind
{
    None,            // Success
    EmptyItemId,     // item_id string is empty
    DuplicateItemId, // append/insert with an already-existing item_id
    ItemNotFound,    // referenced item_id does not exist
    InvalidIndex,    // insert/move index out of range
    NoChange,        // setValue with identical settings
};

struct FormSequenceResult
{
    FormSequenceErrorKind error_kind = FormSequenceErrorKind::None;
    std::string           message;

    [[nodiscard]]
    bool Ok() const noexcept
    {
        return error_kind == FormSequenceErrorKind::None;
    }
};

// ---------------------------------------------------------------------------
// FormSequenceProjector — formSequence ↔ GraphDocumentDto projection
// ---------------------------------------------------------------------------

/// Pure projection + sequence mutation. Stateless: every method is a free
/// function over value types, so it is trivially callable from the plugin
/// boundary (which only sees JSON).
///
/// Projection rules (caller-agnostic — the authoring client never manages
/// edges):
///   - each item  → addNode (node_id == item_id, target + settings preserved)
///   - consecutive items → a single signal edge (edge_type == "signal")
///     connecting them, so the chain order is the node array order
///   - identity metadata (document_id / version / fingerprint) is carried over
///
/// The signal edges use canonical port ids (SignalOutPort / SignalInPort); the
/// GraphCompiler validates signal edges topologically and does NOT require the
/// referenced ports to be declared in any component manifest.
class FormSequenceProjector
{
public:
    /// Canonical signal-edge port ids used by the projection.
    static constexpr std::string_view SignalOutPort = "signal_out";
    static constexpr std::string_view SignalInPort = "signal_in";

    /// Project a formSequence into a GraphDocumentDto (nodes + linear signal
    /// edges). The returned document is ready for GraphCompiler::Compile().
    static Dto::GraphDocumentDto Project(const Dto::FormSequenceDto& sequence);

    /// Apply a single formSequence change to `sequence` in-place.
    /// On failure returns the error kind + message and leaves `sequence`
    /// unmodified.
    static FormSequenceResult ApplyChange(
        Dto::FormSequenceDto&     sequence,
        const FormSequenceChange& change);
};

DAS_CORE_GRAPHRUNTIME_NS_END

// ---------------------------------------------------------------------------
// yyjson (de)serialization support for the formSequence DTOs
// ---------------------------------------------------------------------------
// The aggregate default_caster handles FormSequenceItemDto / FormSequenceDto
// once a DAS_GRAPHRUNTIME_DTO_CASTER specialisation is registered;
// GraphNodeTargetDto already has its custom caster from GraphDocument.h.
#ifndef DAS_GRAPHRUNTIME_DTO_CASTER
#define DAS_GRAPHRUNTIME_DTO_CASTER(DtoType)                                   \
    template <>                                                                \
    struct yyjson::caster<Das::Core::GraphRuntime::Dto::DtoType>               \
    {                                                                          \
        template <yyjson::json_value Json>                                     \
        static Das::Core::GraphRuntime::Dto::DtoType from_json(                \
            const Json& json)                                                  \
        {                                                                      \
            return Das::Core::GraphRuntime::Dto::Detail::CastDtoObject<        \
                Das::Core::GraphRuntime::Dto::DtoType>(json);                  \
        }                                                                      \
    }
#endif

DAS_GRAPHRUNTIME_DTO_CASTER(FormSequenceItemDto);
DAS_GRAPHRUNTIME_DTO_CASTER(FormSequenceDto);

#undef DAS_GRAPHRUNTIME_DTO_CASTER

template <>
struct yyjson::field_name_rule<
    Das::Core::GraphRuntime::Dto::FormSequenceItemDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Dto::FormSequenceDto>
{
    using type = yyjson::snake_to_camel_transform;
};

#endif // DAS_CORE_GRAPHRUNTIME_FORMSEQUENCEPROJECTOR_H

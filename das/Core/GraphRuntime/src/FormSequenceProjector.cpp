#include <das/Core/GraphRuntime/FormSequenceProjector.h>

#include <algorithm>
#include <string>
#include <string_view>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

namespace
{
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /// Find an item index by id. Returns size if not found.
    std::size_t FindItemIndex(
        const Dto::FormSequenceDto& seq,
        const std::string&          item_id)
    {
        for (std::size_t i = 0; i < seq.items.size(); ++i)
        {
            if (seq.items[i].item_id == item_id)
            {
                return i;
            }
        }
        return seq.items.size();
    }

    /// Build the deterministic signal-edge id for a consecutive pair.
    std::string MakeSignalEdgeId(
        const std::string& source_id,
        const std::string& target_id)
    {
        return "sig::" + source_id + "::" + target_id;
    }

    /// Build a graph node that mirrors a sequence item.
    Dto::GraphNodeDto ItemToNode(const Dto::FormSequenceItemDto& item)
    {
        Dto::GraphNodeDto node;
        node.node_id = item.item_id;
        node.target  = item.target;
        node.settings = item.settings;
        return node;
    }

    // -----------------------------------------------------------------------
    // Per-variant handlers
    // -----------------------------------------------------------------------

    FormSequenceResult ValidateNewItem(
        const Dto::FormSequenceDto&      seq,
        const Dto::FormSequenceItemDto&  item)
    {
        if (item.item_id.empty())
        {
            return {FormSequenceErrorKind::EmptyItemId,
                    "item_id must not be empty"};
        }
        if (FindItemIndex(seq, item.item_id) != seq.items.size())
        {
            return {FormSequenceErrorKind::DuplicateItemId,
                    "item_id already exists: " + item.item_id};
        }
        return {FormSequenceErrorKind::None, ""};
    }

    FormSequenceResult HandleAppend(
        Dto::FormSequenceDto&            seq,
        const AppendSequenceItemChange&  change)
    {
        if (auto r = ValidateNewItem(seq, change.item); !r.Ok())
        {
            return r;
        }
        seq.items.push_back(change.item);
        return {FormSequenceErrorKind::None, ""};
    }

    FormSequenceResult HandleInsert(
        Dto::FormSequenceDto&             seq,
        const InsertSequenceItemChange&   change)
    {
        if (change.index > seq.items.size())
        {
            return {FormSequenceErrorKind::InvalidIndex,
                    "insert index out of range"};
        }
        if (auto r = ValidateNewItem(seq, change.item); !r.Ok())
        {
            return r;
        }
        // Rebuild by copy-construction only. yyjson::value move-assignment is
        // unsafe (crashes in yyjson set_value), so avoid vector insert's
        // in-place element shift; push_back copy-constructs, and the final
        // vector move-assign transfers the buffer pointer (no element move).
        std::vector<Dto::FormSequenceItemDto> rebuilt;
        rebuilt.reserve(seq.items.size() + 1);
        for (std::size_t i = 0; i <= seq.items.size(); ++i)
        {
            if (i == change.index)
            {
                rebuilt.push_back(change.item);
            }
            if (i < seq.items.size())
            {
                rebuilt.push_back(seq.items[i]);
            }
        }
        seq.items = std::move(rebuilt);
        return {FormSequenceErrorKind::None, ""};
    }

    FormSequenceResult HandleMove(
        Dto::FormSequenceDto&          seq,
        const MoveSequenceItemChange&  change)
    {
        if (change.from >= seq.items.size() || change.to >= seq.items.size())
        {
            return {FormSequenceErrorKind::InvalidIndex,
                    "move index out of range"};
        }
        if (change.from == change.to)
        {
            return {FormSequenceErrorKind::None, ""};
        }
        // Rebuild in the new order by copy-construction (see HandleInsert for
        // the yyjson move-assignment rationale).
        std::vector<std::size_t> order;
        order.reserve(seq.items.size());
        for (std::size_t i = 0; i < seq.items.size(); ++i)
        {
            if (i != change.from)
            {
                order.push_back(i);
            }
        }
        order.insert(order.begin() + change.to, change.from);

        std::vector<Dto::FormSequenceItemDto> rebuilt;
        rebuilt.reserve(seq.items.size());
        for (const auto idx : order)
        {
            rebuilt.push_back(seq.items[idx]);
        }
        seq.items = std::move(rebuilt);
        return {FormSequenceErrorKind::None, ""};
    }

    FormSequenceResult HandleRemove(
        Dto::FormSequenceDto&            seq,
        const RemoveSequenceItemChange&  change)
    {
        if (change.item_id.empty())
        {
            return {FormSequenceErrorKind::EmptyItemId,
                    "item_id must not be empty"};
        }
        const auto idx = FindItemIndex(seq, change.item_id);
        if (idx == seq.items.size())
        {
            return {FormSequenceErrorKind::ItemNotFound,
                    "item not found: " + change.item_id};
        }
        // Rebuild by copy-construction, skipping the removed item (see
        // HandleInsert for the yyjson move-assignment rationale).
        std::vector<Dto::FormSequenceItemDto> rebuilt;
        rebuilt.reserve(seq.items.size() - 1);
        for (std::size_t i = 0; i < seq.items.size(); ++i)
        {
            if (i != idx)
            {
                rebuilt.push_back(seq.items[i]);
            }
        }
        seq.items = std::move(rebuilt);
        return {FormSequenceErrorKind::None, ""};
    }

    FormSequenceResult HandleSetValue(
        Dto::FormSequenceDto&                seq,
        const SetSequenceItemValueChange&    change)
    {
        if (change.item_id.empty())
        {
            return {FormSequenceErrorKind::EmptyItemId,
                    "item_id must not be empty"};
        }
        const auto idx = FindItemIndex(seq, change.item_id);
        if (idx == seq.items.size())
        {
            return {FormSequenceErrorKind::ItemNotFound,
                    "item not found: " + change.item_id};
        }
        seq.items[idx].settings = change.settings;
        return {FormSequenceErrorKind::None, ""};
    }
} // anonymous namespace

// ---------------------------------------------------------------------------
// Project — formSequence → GraphDocumentDto
// ---------------------------------------------------------------------------

Dto::GraphDocumentDto FormSequenceProjector::Project(
    const Dto::FormSequenceDto& sequence)
{
    Dto::GraphDocumentDto doc;
    doc.document_id = sequence.document_id;
    doc.version     = sequence.version;
    doc.fingerprint = sequence.fingerprint;

    doc.nodes.reserve(sequence.items.size());
    doc.edges.reserve(sequence.items.empty() ? 0 : sequence.items.size() - 1);

    for (const auto& item : sequence.items)
    {
        doc.nodes.push_back(ItemToNode(item));
    }

    // Linear signal chain: connect each item to its successor. The authoring
    // client never sees these edges — they are auto-maintained by the
    // projection so the chain order is the node array order.
    for (std::size_t i = 1; i < sequence.items.size(); ++i)
    {
        const auto& src = sequence.items[i - 1].item_id;
        const auto& tgt = sequence.items[i].item_id;

        Dto::GraphEdgeDto edge;
        edge.edge_id        = MakeSignalEdgeId(src, tgt);
        edge.source_node_id = src;
        edge.source_port_id = std::string(SignalOutPort);
        edge.target_node_id = tgt;
        edge.target_port_id = std::string(SignalInPort);
        edge.edge_type      = "signal";
        doc.edges.push_back(std::move(edge));
    }

    return doc;
}

// ---------------------------------------------------------------------------
// ApplyChange — visitor dispatch
// ---------------------------------------------------------------------------

FormSequenceResult FormSequenceProjector::ApplyChange(
    Dto::FormSequenceDto&     sequence,
    const FormSequenceChange& change)
{
    return std::visit(
        [&sequence](const auto& payload) -> FormSequenceResult
        {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, AppendSequenceItemChange>)
            {
                return HandleAppend(sequence, payload);
            }
            else if constexpr (std::is_same_v<T, InsertSequenceItemChange>)
            {
                return HandleInsert(sequence, payload);
            }
            else if constexpr (std::is_same_v<T, MoveSequenceItemChange>)
            {
                return HandleMove(sequence, payload);
            }
            else if constexpr (std::is_same_v<T, RemoveSequenceItemChange>)
            {
                return HandleRemove(sequence, payload);
            }
            else if constexpr (std::is_same_v<T, SetSequenceItemValueChange>)
            {
                return HandleSetValue(sequence, payload);
            }
        },
        change.payload);
}

DAS_CORE_GRAPHRUNTIME_NS_END

#include <das/Core/GraphRuntime/RepositoryInvokeGraph.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Das::Core::GraphRuntime::RepositoryInvoke
{
    namespace
    {
        // ----------------------------------------------------------------
        // Cycle validation (moved verbatim from TaskScheduler
        // RepositoryInvokeGraph.cpp — behavior-equivalence contract).
        // ----------------------------------------------------------------
        enum class Color
        {
            White,
            Gray,
            Black,
        };

        struct Frame
        {
            int64_t                         entry_id = 0;
            std::size_t                     next_edge_index = 0;
            const RepositoryDependencyEdge* incoming_edge = nullptr;
        };

        using AdjacencyList =
            std::map<int64_t, std::vector<const RepositoryDependencyEdge*>>;
        using ColorMap = std::map<int64_t, Color>;

        AdjacencyList BuildAdjacency(
            std::span<const RepositoryDependencyEdge> edges)
        {
            AdjacencyList adjacency;
            for (const auto& edge : edges)
            {
                adjacency[edge.source_entry_id].push_back(&edge);
                adjacency.try_emplace(edge.target_entry_id);
            }

            for (auto& [_, outgoing_edges] : adjacency)
            {
                std::ranges::sort(
                    outgoing_edges,
                    [](const auto* lhs, const auto* rhs)
                    {
                        if (lhs->target_entry_id != rhs->target_entry_id)
                        {
                            return lhs->target_entry_id < rhs->target_entry_id;
                        }
                        return lhs->source_node_id < rhs->source_node_id;
                    });
            }

            return adjacency;
        }

        Color GetColor(const ColorMap& colors, int64_t entry_id)
        {
            const auto iterator = colors.find(entry_id);
            return iterator == colors.end() ? Color::White : iterator->second;
        }

        RepositoryCyclePathItem MakeCyclePathItem(
            int64_t                         entry_id,
            const RepositoryDependencyEdge* outgoing_edge)
        {
            RepositoryCyclePathItem item;
            item.entry_id = entry_id;
            if (outgoing_edge != nullptr)
            {
                item.source_node_id = outgoing_edge->source_node_id;
                item.source_node_label = outgoing_edge->source_node_label;
            }
            return item;
        }

        RepositoryAcyclicValidationResult BuildCycleResult(
            const std::vector<Frame>&       stack,
            const RepositoryDependencyEdge& closing_edge)
        {
            const auto cycle_start_iterator = std::ranges::find_if(
                stack,
                [&closing_edge](const auto& frame)
                { return frame.entry_id == closing_edge.target_entry_id; });

            RepositoryAcyclicValidationResult result;
            result.ok = false;

            // The active stack stores incoming edges, so each cycle item uses
            // the next frame's incoming edge as its outgoing source metadata.
            for (auto index = static_cast<std::size_t>(
                     cycle_start_iterator - stack.begin());
                 index < stack.size();
                 ++index)
            {
                const auto* outgoing_edge = index + 1 < stack.size()
                                                ? stack[index + 1].incoming_edge
                                                : &closing_edge;
                result.cycle_path.push_back(
                    MakeCyclePathItem(stack[index].entry_id, outgoing_edge));
            }

            result.cycle_path.push_back(
                MakeCyclePathItem(closing_edge.target_entry_id, nullptr));
            return result;
        }

        RepositoryAcyclicValidationResult FindCycleFromRoot(
            int64_t              root_entry_id,
            const AdjacencyList& adjacency,
            ColorMap&            colors)
        {
            std::vector<Frame> stack;
            stack.push_back({.entry_id = root_entry_id});
            colors[root_entry_id] = Color::Gray;

            while (!stack.empty())
            {
                auto& frame = stack.back();

                const auto adjacency_iterator = adjacency.find(frame.entry_id);
                if (adjacency_iterator == adjacency.end()
                    || frame.next_edge_index
                           >= adjacency_iterator->second.size())
                {
                    colors[frame.entry_id] = Color::Black;
                    stack.pop_back();
                    continue;
                }

                const auto* edge =
                    adjacency_iterator->second[frame.next_edge_index];
                ++frame.next_edge_index;

                const auto target_color =
                    GetColor(colors, edge->target_entry_id);
                if (target_color == Color::White)
                {
                    colors[edge->target_entry_id] = Color::Gray;
                    stack.push_back(
                        {.entry_id = edge->target_entry_id,
                         .incoming_edge = edge});
                    continue;
                }

                if (target_color == Color::Gray)
                {
                    return BuildCycleResult(stack, *edge);
                }
            }

            return {};
        }

        // ----------------------------------------------------------------
        // Edge extraction (moved verbatim from TaskScheduler
        // RepositoryInvokeCompiler.cpp — behavior-equivalence contract).
        // ----------------------------------------------------------------
        template <typename ObjectRef>
        std::optional<int64_t> GetIntField(
            const ObjectRef& object,
            std::string_view key)
        {
            if (!object.contains(key))
            {
                return std::nullopt;
            }
            return object[key].as_sint();
        }

        template <typename ObjectRef>
        std::optional<std::string> GetStringField(
            const ObjectRef& object,
            std::string_view key)
        {
            if (!object.contains(key))
            {
                return std::nullopt;
            }
            auto value = object[key].as_string();
            if (!value)
            {
                return std::nullopt;
            }
            return std::string(*value);
        }

        template <typename ObjectRef>
        std::optional<std::string> FirstStringField(
            const ObjectRef& object,
            std::string_view first,
            std::string_view second)
        {
            if (auto value = GetStringField(object, first))
            {
                return value;
            }
            return GetStringField(object, second);
        }

        template <typename ObjectRef>
        void AddEdgeFromRepositoryRef(
            const ObjectRef&                       ref,
            int64_t                                source_entry_id,
            const std::optional<std::string>&      source_node_id,
            const std::optional<std::string>&      source_node_label,
            std::vector<RepositoryDependencyEdge>& edges)
        {
            auto kind = GetStringField(ref, "kind");
            if (kind && *kind != "taskRepositoryRef")
            {
                return;
            }

            auto target_entry_id = GetIntField(ref, "entryId");
            if (!target_entry_id)
            {
                return;
            }

            RepositoryDependencyEdge edge;
            edge.source_entry_id = source_entry_id;
            edge.target_entry_id = *target_entry_id;
            edge.source_node_id = source_node_id;
            edge.source_node_label = source_node_label;
            edges.push_back(std::move(edge));
        }

        template <typename JsonValue>
        void ExtractEdgesRecursive(
            int64_t                                inherited_source_entry_id,
            const JsonValue&                       value,
            std::optional<std::string>             inherited_node_id,
            std::optional<std::string>             inherited_node_label,
            std::vector<RepositoryDependencyEdge>& edges)
        {
            if (auto array = value.as_array())
            {
                for (const auto& item : *array)
                {
                    ExtractEdgesRecursive(
                        inherited_source_entry_id,
                        item,
                        inherited_node_id,
                        inherited_node_label,
                        edges);
                }
                return;
            }

            auto object = value.as_object();
            if (!object)
            {
                return;
            }

            auto source_entry_id = inherited_source_entry_id;
            if (auto source_override = GetIntField(*object, "sourceEntryId"))
            {
                source_entry_id = *source_override;
            }

            auto source_node_id = FirstStringField(*object, "id", "nodeId");
            if (!source_node_id)
            {
                source_node_id = inherited_node_id;
            }
            auto source_node_label = FirstStringField(*object, "label", "name");
            if (!source_node_label)
            {
                source_node_label = inherited_node_label;
            }

            for (auto key :
                 {std::string_view("taskRepositoryRef"),
                  std::string_view("repositoryRef")})
            {
                if (!object->contains(key))
                {
                    continue;
                }
                // Reference values ride the wire either as the wrapped
                // object ({kind, entryId}) or as a plain entryId integer
                // (the only shape settings writers emit today). Both shapes
                // must join the dependency graph, otherwise cycles behind
                // integer refs are invisible to cycle detection.
                if (auto integer_ref = (*object)[key].as_sint())
                {
                    RepositoryDependencyEdge edge;
                    edge.source_entry_id = source_entry_id;
                    edge.target_entry_id = *integer_ref;
                    edge.source_node_id = source_node_id;
                    edge.source_node_label = source_node_label;
                    edges.push_back(std::move(edge));
                    continue;
                }
                auto ref = (*object)[key].as_object();
                if (ref)
                {
                    AddEdgeFromRepositoryRef(
                        *ref,
                        source_entry_id,
                        source_node_id,
                        source_node_label,
                        edges);
                }
            }

            for (const auto& [_, child] : *object)
            {
                ExtractEdgesRecursive(
                    source_entry_id,
                    child,
                    source_node_id,
                    source_node_label,
                    edges);
            }
        }
    } // namespace

    RepositoryAcyclicValidationResult ValidateRepositoryInvokeAcyclic(
        std::span<const RepositoryDependencyEdge> edges)
    {
        const auto adjacency = BuildAdjacency(edges);

        ColorMap colors;

        for (const auto& [entry_id, _] : adjacency)
        {
            if (GetColor(colors, entry_id) != Color::White)
            {
                continue;
            }

            auto result = FindCycleFromRoot(entry_id, adjacency, colors);
            if (!result.ok)
            {
                return result;
            }
        }

        return {};
    }

    std::vector<RepositoryDependencyEdge>
    ExtractRepositoryInvokeDependencyEdges(
        int64_t              source_entry_id,
        const yyjson::value& source_graph)
    {
        std::vector<RepositoryDependencyEdge> edges;
        ExtractEdgesRecursive(
            source_entry_id,
            source_graph,
            std::nullopt,
            std::nullopt,
            edges);
        return edges;
    }
} // namespace Das::Core::GraphRuntime::RepositoryInvoke

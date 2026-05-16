#include <das/Core/TaskScheduler/RepositoryInvokeGraph.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace Das::Core::TaskScheduler::RepositoryInvoke
{
    namespace
    {
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
            int64_t entry_id,
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
            const std::vector<Frame>&         stack,
            const RepositoryDependencyEdge& closing_edge)
        {
            const auto cycle_start_iterator = std::ranges::find_if(
                stack,
                [&closing_edge](const auto& frame)
                {
                    return frame.entry_id == closing_edge.target_entry_id;
                });

            RepositoryAcyclicValidationResult result;
            result.ok = false;

            // The active stack stores incoming edges, so each cycle item uses
            // the next frame's incoming edge as its outgoing source metadata.
            for (auto index =
                     static_cast<std::size_t>(
                         cycle_start_iterator - stack.begin());
                 index < stack.size();
                 ++index)
            {
                const auto* outgoing_edge =
                    index + 1 < stack.size() ? stack[index + 1].incoming_edge
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
                if (adjacency_iterator == adjacency.end() ||
                    frame.next_edge_index >=
                        adjacency_iterator->second.size())
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
} // namespace Das::Core::TaskScheduler::RepositoryInvoke

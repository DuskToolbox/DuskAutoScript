#include <das/Core/GraphRuntime/GraphAuthoring.h>

#include <algorithm>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

namespace
{
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /// Find a node by id. Returns nullptr if not found.
    Dto::GraphNodeDto* FindNode(
        Dto::GraphDocumentDto& doc,
        const std::string&     node_id)
    {
        for (auto& node : doc.nodes)
        {
            if (node.node_id == node_id)
            {
                return &node;
            }
        }
        return nullptr;
    }

    /// Find an edge by id. Returns nullptr if not found.
    Dto::GraphEdgeDto* FindEdge(
        Dto::GraphDocumentDto& doc,
        const std::string&     edge_id)
    {
        for (auto& edge : doc.edges)
        {
            if (edge.edge_id == edge_id)
            {
                return &edge;
            }
        }
        return nullptr;
    }

    /// Remove all edges connected to a given node_id.
    void RemoveEdgesForNode(
        Dto::GraphDocumentDto& doc,
        const std::string&     node_id)
    {
        doc.edges.erase(
            std::remove_if(
                doc.edges.begin(),
                doc.edges.end(),
                [&](const Dto::GraphEdgeDto& e)
                {
                    return e.source_node_id == node_id
                           || e.target_node_id == node_id;
                }),
            doc.edges.end());
    }

    // -----------------------------------------------------------------------
    // Per-variant handlers
    // -----------------------------------------------------------------------

    AuthoringResult HandleAddNode(
        Dto::GraphDocumentDto& doc,
        const AddNodeChange&   change)
    {
        const auto& node_id = change.node.node_id;

        if (node_id.empty())
        {
            return {
                AuthoringErrorKind::EmptyNodeId,
                "node_id must not be empty"};
        }

        if (FindNode(doc, node_id) != nullptr)
        {
            return {
                AuthoringErrorKind::DuplicateNodeId,
                "node_id already exists: " + node_id};
        }

        doc.nodes.push_back(change.node);
        return {AuthoringErrorKind::None, ""};
    }

    AuthoringResult HandleRemoveNode(
        Dto::GraphDocumentDto&  doc,
        const RemoveNodeChange& change)
    {
        const auto& node_id = change.node_id;

        if (node_id.empty())
        {
            return {
                AuthoringErrorKind::EmptyNodeId,
                "node_id must not be empty"};
        }

        if (FindNode(doc, node_id) == nullptr)
        {
            return {
                AuthoringErrorKind::NodeNotFound,
                "node not found: " + node_id};
        }

        // Cascade — remove all edges connected to this node first.
        RemoveEdgesForNode(doc, node_id);

        // Remove the node itself.
        doc.nodes.erase(
            std::remove_if(
                doc.nodes.begin(),
                doc.nodes.end(),
                [&](const Dto::GraphNodeDto& n)
                { return n.node_id == node_id; }),
            doc.nodes.end());

        return {AuthoringErrorKind::None, ""};
    }

    AuthoringResult HandleConnectPorts(
        Dto::GraphDocumentDto&    doc,
        const ConnectPortsChange& change)
    {
        const auto& edge_id = change.edge.edge_id;

        if (edge_id.empty())
        {
            return {
                AuthoringErrorKind::EmptyEdgeId,
                "edge_id must not be empty"};
        }

        if (FindEdge(doc, edge_id) != nullptr)
        {
            return {
                AuthoringErrorKind::DuplicateEdgeId,
                "edge_id already exists: " + edge_id};
        }

        // Validate that source and target nodes exist.
        if (FindNode(doc, change.edge.source_node_id) == nullptr)
        {
            return {
                AuthoringErrorKind::InvalidEdge,
                "source node not found: " + change.edge.source_node_id};
        }

        if (FindNode(doc, change.edge.target_node_id) == nullptr)
        {
            return {
                AuthoringErrorKind::InvalidEdge,
                "target node not found: " + change.edge.target_node_id};
        }

        doc.edges.push_back(change.edge);
        return {AuthoringErrorKind::None, ""};
    }

    AuthoringResult HandleDisconnectPorts(
        Dto::GraphDocumentDto&       doc,
        const DisconnectPortsChange& change)
    {
        const auto& edge_id = change.edge_id;

        if (edge_id.empty())
        {
            return {
                AuthoringErrorKind::EmptyEdgeId,
                "edge_id must not be empty"};
        }

        if (FindEdge(doc, edge_id) == nullptr)
        {
            return {
                AuthoringErrorKind::EdgeNotFound,
                "edge not found: " + edge_id};
        }

        doc.edges.erase(
            std::remove_if(
                doc.edges.begin(),
                doc.edges.end(),
                [&](const Dto::GraphEdgeDto& e)
                { return e.edge_id == edge_id; }),
            doc.edges.end());

        return {AuthoringErrorKind::None, ""};
    }

    AuthoringResult HandleUpdateNodeConfig(
        Dto::GraphDocumentDto&        doc,
        const UpdateNodeConfigChange& change)
    {
        const auto& node_id = change.node_id;

        if (node_id.empty())
        {
            return {
                AuthoringErrorKind::EmptyNodeId,
                "node_id must not be empty"};
        }

        auto* node = FindNode(doc, node_id);
        if (node == nullptr)
        {
            return {
                AuthoringErrorKind::NodeNotFound,
                "node not found: " + node_id};
        }

        node->settings = change.settings;
        return {AuthoringErrorKind::None, ""};
    }

} // anonymous namespace

// ---------------------------------------------------------------------------
// ApplySettingsChange — visitor dispatch
// ---------------------------------------------------------------------------

AuthoringResult ApplySettingsChange(
    Dto::GraphDocumentDto&      document,
    const GraphAuthoringChange& change)
{
    return std::visit(
        [&document](const auto& payload) -> AuthoringResult
        {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, AddNodeChange>)
            {
                return HandleAddNode(document, payload);
            }
            else if constexpr (std::is_same_v<T, RemoveNodeChange>)
            {
                return HandleRemoveNode(document, payload);
            }
            else if constexpr (std::is_same_v<T, ConnectPortsChange>)
            {
                return HandleConnectPorts(document, payload);
            }
            else if constexpr (std::is_same_v<T, DisconnectPortsChange>)
            {
                return HandleDisconnectPorts(document, payload);
            }
            else if constexpr (std::is_same_v<T, UpdateNodeConfigChange>)
            {
                return HandleUpdateNodeConfig(document, payload);
            }
        },
        change.payload);
}

// ---------------------------------------------------------------------------
// LintFromSequenceLinearity — optional, non-blocking (DAS-75)
// ---------------------------------------------------------------------------

namespace
{
    bool HasFromSequenceTag(const Dto::GraphDocumentDto& doc)
    {
        for (const auto& tag : doc.tags)
        {
            if (tag == "fromsequence")
            {
                return true;
            }
        }
        return false;
    }
} // namespace

std::vector<FromSequenceLintWarning> LintFromSequenceLinearity(
    const Dto::GraphDocumentDto& document)
{
    std::vector<FromSequenceLintWarning> warnings;

    // No tag → no opinion. The lint only applies to fromsequence authoring.
    if (!HasFromSequenceTag(document) || document.nodes.empty())
    {
        return warnings;
    }

    // Signal-edge in/out degree per node (only signal edges define the chain).
    std::unordered_map<std::string, int> signal_in;
    std::unordered_map<std::string, int> signal_out;
    for (const auto& node : document.nodes)
    {
        signal_in[node.node_id] = 0;
        signal_out[node.node_id] = 0;
    }
    for (const auto& edge : document.edges)
    {
        if (edge.edge_type != "signal")
        {
            continue;
        }
        if (signal_in.count(edge.target_node_id) > 0)
        {
            signal_in[edge.target_node_id]++;
        }
        if (signal_out.count(edge.source_node_id) > 0)
        {
            signal_out[edge.source_node_id]++;
        }
    }

    // A linear chain allows at most one signal in and one signal out per node.
    for (const auto& node : document.nodes)
    {
        const auto& id = node.node_id;
        if (signal_out[id] > 1)
        {
            warnings.push_back(
                {id,
                 "fans out via " + std::to_string(signal_out[id])
                     + " signal edges — a fromsequence chain should be linear"});
        }
        if (signal_in[id] > 1)
        {
            warnings.push_back(
                {id,
                 "merges " + std::to_string(signal_in[id])
                     + " signal edges — a fromsequence chain should be linear"});
        }
    }

    // Connected components of the signal-edge subgraph (undirected). A node that
    // touches no signal edge is isolated; multiple components (incl. isolated
    // nodes) mean the topology is not a single chain.
    std::unordered_map<std::string, std::vector<std::string>> signal_adj;
    for (const auto& edge : document.edges)
    {
        if (edge.edge_type != "signal")
        {
            continue;
        }
        if (signal_in.count(edge.source_node_id) > 0
            && signal_in.count(edge.target_node_id) > 0)
        {
            signal_adj[edge.source_node_id].push_back(edge.target_node_id);
            signal_adj[edge.target_node_id].push_back(edge.source_node_id);
        }
    }

    std::unordered_set<std::string> visited;
    int                             components = 0;
    for (const auto& node : document.nodes)
    {
        if (visited.count(node.node_id) > 0)
        {
            continue;
        }
        // An isolated node (no signal edge at all) is its own disconnected piece.
        if (signal_in[node.node_id] == 0 && signal_out[node.node_id] == 0)
        {
            visited.insert(node.node_id);
            ++components;
            continue;
        }
        ++components;
        std::deque<std::string> queue{node.node_id};
        visited.insert(node.node_id);
        while (!queue.empty())
        {
            std::string u = queue.front();
            queue.pop_front();
            for (const auto& v : signal_adj[u])
            {
                if (visited.insert(v).second)
                {
                    queue.push_back(v);
                }
            }
        }
    }

    if (components > 1)
    {
        warnings.push_back(
            {"",
             "signal topology splits into " + std::to_string(components)
                 + " disconnected pieces — expected a single linear chain"});
    }

    return warnings;
}

DAS_CORE_GRAPHRUNTIME_NS_END

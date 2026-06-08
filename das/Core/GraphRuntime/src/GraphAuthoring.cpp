#include <das/Core/GraphRuntime/GraphAuthoring.h>

#include <algorithm>
#include <string_view>

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

DAS_CORE_GRAPHRUNTIME_NS_END

#include "RoutingGraph.h"
#include <algorithm>
#include <fstream>
#include <sstream>

RoutingGraph::RoutingGraph() = default;

int RoutingGraph::addNode(const std::string& name,
                          const std::string& deviceId,
                          DeviceType         type)
{
    std::lock_guard<std::mutex> lk(mutex);
    RoutingNode n;
    n.id       = m_nextNodeId++;
    n.name     = name;
    n.deviceId = deviceId;
    n.type     = type;

    // Create pins based on type
    bool hasIn  = (type == DeviceType::WasapiCapture  ||
                   type == DeviceType::WasapiLoopback ||
                   type == DeviceType::AsioInput       ||
                   type == DeviceType::VirtualSink     ||
                   type == DeviceType::Mixer);
    bool hasOut = (type == DeviceType::WasapiRender   ||
                   type == DeviceType::WasapiCapture  ||
                   type == DeviceType::WasapiLoopback ||
                   type == DeviceType::AsioInput       ||
                   type == DeviceType::AsioOutput      ||
                   type == DeviceType::VirtualSink     ||
                   type == DeviceType::Mixer);

    // Input devices: output pin (they produce signal)
    if (type == DeviceType::WasapiCapture  ||
        type == DeviceType::WasapiLoopback ||
        type == DeviceType::AsioInput) {
        n.outputs.push_back({n.id, 0, false});
    }
    // Output/sink devices: input pin (they consume signal)
    if (type == DeviceType::WasapiRender  ||
        type == DeviceType::AsioOutput    ||
        type == DeviceType::VirtualSink) {
        n.inputs.push_back({n.id, 0, true});
    }
    // Mixer: multiple inputs, one output
    if (type == DeviceType::Mixer) {
        for (int i = 0; i < 4; ++i)
            n.inputs.push_back({n.id, i, true});
        n.outputs.push_back({n.id, 0, false});
    }

    m_nodes.push_back(std::move(n));
    m_dirty = true;
    return m_nodes.back().id;
}

void RoutingGraph::removeNode(int nodeId)
{
    std::lock_guard<std::mutex> lk(mutex);
    // Remove all edges connected to this node
    m_edges.erase(
        std::remove_if(m_edges.begin(), m_edges.end(),
            [nodeId](const RoutingEdge& e){
                return e.fromNodeId == nodeId || e.toNodeId == nodeId;
            }),
        m_edges.end());
    // Remove node
    m_nodes.erase(
        std::remove_if(m_nodes.begin(), m_nodes.end(),
            [nodeId](const RoutingNode& n){ return n.id == nodeId; }),
        m_nodes.end());
    m_dirty = true;
}

int RoutingGraph::addEdge(int fromNode, int fromPin,
                          int toNode,   int toPin)
{
    std::lock_guard<std::mutex> lk(mutex);
    // No duplicate edges
    for (auto& e : m_edges)
        if (e.fromNodeId == fromNode && e.toNodeId == toNode &&
            e.fromPinIndex == fromPin && e.toPinIndex == toPin)
            return e.id;

    RoutingEdge edge;
    edge.id           = m_nextEdgeId++;
    edge.fromNodeId   = fromNode;
    edge.fromPinIndex = fromPin;
    edge.toNodeId     = toNode;
    edge.toPinIndex   = toPin;
    m_edges.push_back(edge);
    m_dirty = true;
    return edge.id;
}

void RoutingGraph::removeEdge(int edgeId)
{
    std::lock_guard<std::mutex> lk(mutex);
    m_edges.erase(
        std::remove_if(m_edges.begin(), m_edges.end(),
            [edgeId](const RoutingEdge& e){ return e.id == edgeId; }),
        m_edges.end());
    m_dirty = true;
}

RoutingNode* RoutingGraph::findNode(int id) {
    for (auto& n : m_nodes) if (n.id == id) return &n;
    return nullptr;
}
const RoutingNode* RoutingGraph::findNode(int id) const {
    for (auto& n : m_nodes) if (n.id == id) return &n;
    return nullptr;
}
RoutingEdge* RoutingGraph::findEdge(int id) {
    for (auto& e : m_edges) if (e.id == id) return &e;
    return nullptr;
}

std::vector<const RoutingEdge*> RoutingGraph::incomingEdges(int nodeId) const {
    std::vector<const RoutingEdge*> result;
    for (auto& e : m_edges)
        if (e.toNodeId == nodeId) result.push_back(&e);
    return result;
}
std::vector<const RoutingEdge*> RoutingGraph::outgoingEdges(int nodeId) const {
    std::vector<const RoutingEdge*> result;
    for (auto& e : m_edges)
        if (e.fromNodeId == nodeId) result.push_back(&e);
    return result;
}

void RoutingGraph::clear() {
    std::lock_guard<std::mutex> lk(mutex);
    m_nodes.clear();
    m_edges.clear();
    m_dirty = true;
}

bool RoutingGraph::saveToFile(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    std::lock_guard<std::mutex> lk(mutex);
    f << "[nodes]\n";
    for (auto& n : m_nodes)
        f << n.id << "," << (int)n.type << ","
          << n.posX << "," << n.posY << ","
          << n.gainDb << ","
          << n.deviceId << ","
          << n.name << ","
          << n.noiseFilterMode << ","
          << n.noiseGateThreshDb << ","
          << n.deviceChannels << ","
          << n.deviceSampleRate << ","
          << (n.asioMinBuffer ? 1 : 0) << "\n";
    f << "[edges]\n";
    for (auto& e : m_edges)
        f << e.id << "," << e.fromNodeId << "," << e.fromPinIndex << ","
          << e.toNodeId << "," << e.toPinIndex << "\n";
    return true;
}

bool RoutingGraph::loadFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    clear();
    std::lock_guard<std::mutex> lk(mutex);
    std::string line, section;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line[0] == '[') { section = line; continue; }
        std::istringstream ss(line);
        std::string tok;
        std::vector<std::string> parts;
        while (std::getline(ss, tok, ',')) parts.push_back(tok);
        if (section == "[nodes]" && parts.size() >= 7) {
            RoutingNode n;
            n.id       = std::stoi(parts[0]);
            n.type     = (DeviceType)std::stoi(parts[1]);
            n.posX     = std::stof(parts[2]);
            n.posY     = std::stof(parts[3]);
            n.gainDb   = std::stof(parts[4]);
            n.deviceId = parts[5];
            n.name     = parts[6];
            // Restore extra fields if present
            if (parts.size() >= 9) {
                n.noiseFilterMode    = std::stoi(parts[7]);
                n.noiseGateThreshDb  = std::stof(parts[8]);
            }
            if (parts.size() >= 12) {
                n.deviceChannels   = std::stoi(parts[9]);
                n.deviceSampleRate = std::stoi(parts[10]);
                n.asioMinBuffer    = (std::stoi(parts[11]) != 0);
            }
            if (n.id >= m_nextNodeId) m_nextNodeId = n.id + 1;

            // ── Rebuild pins (same logic as addNode) ───────────────────────
            if (n.type == DeviceType::WasapiCapture  ||
                n.type == DeviceType::WasapiLoopback ||
                n.type == DeviceType::AsioInput) {
                n.outputs.push_back({n.id, 0, false});
            }
            if (n.type == DeviceType::WasapiRender ||
                n.type == DeviceType::AsioOutput   ||
                n.type == DeviceType::VirtualSink) {
                n.inputs.push_back({n.id, 0, true});
            }
            if (n.type == DeviceType::Mixer) {
                for (int i = 0; i < 4; ++i)
                    n.inputs.push_back({n.id, i, true});
                n.outputs.push_back({n.id, 0, false});
            }

            m_nodes.push_back(std::move(n));
        } else if (section == "[edges]" && parts.size() >= 5) {
            RoutingEdge e;
            e.id           = std::stoi(parts[0]);
            e.fromNodeId   = std::stoi(parts[1]);
            e.fromPinIndex = std::stoi(parts[2]);
            e.toNodeId     = std::stoi(parts[3]);
            e.toPinIndex   = std::stoi(parts[4]);
            if (e.id >= m_nextEdgeId) m_nextEdgeId = e.id + 1;
            m_edges.push_back(e);
        }
    }
    return true;
}

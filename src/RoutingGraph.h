#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <memory>

// Forward-declare to avoid heavyweight include in graph header
class NoiseFilter;

// ============================================================================
//  Routing Graph  –  nodes = audio devices, edges = signal paths
// ============================================================================

enum class DeviceType {
    WasapiCapture,      // microphone / line-in
    WasapiLoopback,     // what's-playing loopback
    WasapiRender,       // speakers / headphones
    AsioInput,
    AsioOutput,
    VirtualSink,        // VB-Cable or similar
    Mixer,              // internal mix node (many→one)
};

enum class DeviceState {
    Idle,
    Active,
    Error,
};

struct AudioPin {
    int     nodeId;
    int     pinIndex;   // 0-based channel pair
    bool    isInput;    // true = accepts signal
};

struct RoutingNode {
    int         id;
    std::string name;           // display name
    std::string deviceId;       // system device ID
    DeviceType  type;
    DeviceState state   { DeviceState::Idle };
    bool        enabled { true };
    float       gainDb  { 0.0f };
    float       peakL   { 0.0f };
    float       peakR   { 0.0f };
    // visual position in node editor
    float       posX { 0.0f };
    float       posY { 0.0f };

    // Noise suppression (only applied to WasapiCapture / AsioInput nodes)
    int         noiseFilterMode { 0 };   // 0=Off 1=Light 2=Medium 3=Aggressive
    float       noiseGateThreshDb{ -38.0f };

    // Device format overrides (0 = use system default)
    int         deviceChannels  { 0 };   // 0=auto, 1=mono, 2=stereo
    int         deviceSampleRate{ 0 };   // 0=auto, 44100, 48000, etc.
    bool        asioMinBuffer   { true }; // ASIO: use minimum buffer size for lowest latency

    // Runtime info (filled when stream is opened, not serialized)
    int         rtSampleRate    { 0 };   // actual sample rate of opened stream
    int         rtBitDepth      { 0 };   // actual bit depth
    int         rtChannels      { 0 };   // actual channel count

    // Runtime noise filter instance (not serialized — created on demand by AudioEngine)
    std::shared_ptr<NoiseFilter> noiseFilter;

    std::vector<AudioPin> inputs;
    std::vector<AudioPin> outputs;
};

struct RoutingEdge {
    int id;
    int fromNodeId;
    int fromPinIndex;
    int toNodeId;
    int toPinIndex;
};

class RoutingGraph {
public:
    RoutingGraph();

    int  addNode(const std::string& name, const std::string& deviceId, DeviceType type);
    void removeNode(int nodeId);
    int  addEdge(int fromNode, int fromPin, int toNode, int toPin);
    void removeEdge(int edgeId);

    RoutingNode*        findNode(int id);
    const RoutingNode*  findNode(int id) const;
    RoutingEdge*        findEdge(int id);

    std::vector<RoutingNode>&       nodes()  { return m_nodes; }
    std::vector<RoutingEdge>&       edges()  { return m_edges; }
    const std::vector<RoutingNode>& nodes()  const { return m_nodes; }
    const std::vector<RoutingEdge>& edges()  const { return m_edges; }

    // Returns all edges going INTO a node
    std::vector<const RoutingEdge*> incomingEdges(int nodeId) const;
    // Returns all edges going OUT of a node
    std::vector<const RoutingEdge*> outgoingEdges(int nodeId) const;

    void  clear();
    bool  isDirty() const { return m_dirty; }
    void  clearDirty()    { m_dirty = false; }

    // Serialisation (simple INI-like format)
    bool saveToFile(const std::string& path) const;
    bool loadFromFile(const std::string& path);

    mutable std::mutex mutex;

private:
    std::vector<RoutingNode> m_nodes;
    std::vector<RoutingEdge> m_edges;
    int   m_nextNodeId { 1 };
    int   m_nextEdgeId { 1 };
    bool  m_dirty      { false };
};

#pragma once
#include "RoutingGraph.h"
#include "AudioEngine.h"
#include "VolumeMixer.h"
#include "imgui.h"
#include "imnodes.h"

#include <string>
#include <functional>
#include <set>
#include <unordered_map>

// ============================================================================
//  Node editor pin ID encoding
// ============================================================================
inline int encodePinId(int nodeId, int pinIndex, bool isInput) {
    return (nodeId & 0x3FFF) | ((pinIndex & 0x1F) << 14) | (isInput ? (1<<19) : 0);
}
inline void decodePinId(int id, int& nodeId, int& pinIndex, bool& isInput) {
    nodeId   = id & 0x3FFF;
    pinIndex = (id >> 14) & 0x1F;
    isInput  = (id >> 19) & 1;
}

// ============================================================================
//  NodeEditorUI
// ============================================================================
class NodeEditorUI {
public:
    NodeEditorUI();
    ~NodeEditorUI();

    void init(RoutingGraph* graph, AudioEngine* engine);
    void shutdown();
    void renderFrame(HWND hwnd);

private:
    void drawMenuBar(HWND hwnd);
    void drawDevicePanel();
    void drawNodeEditor();
    void drawPropertiesPanel();
    void drawStatusBar();
    void drawSettingsWindow();

    void drawWasapiCaptureNode (RoutingNode& n);
    void drawWasapiRenderNode  (RoutingNode& n);
    void drawMixerNode         (RoutingNode& n);
    void drawAsioNode          (RoutingNode& n, bool isInput);
    void drawVirtualDeviceNode (RoutingNode& n);

    void drawMiniMeter(float peak, float width = 80.0f);
    void showAddDevicePopup();
    void syncPositionsToGraph();
    void syncPositionsFromGraph();

    // ── Members ─────────────────────────────────────────────────────────────
    RoutingGraph* m_graph  { nullptr };
    AudioEngine*  m_engine { nullptr };

    ImNodesContext* m_imnodesCtx { nullptr };

    bool  m_showAddPopup       { false };
    bool  m_showAsioPanel      { false };
    bool  m_showUpdater        { false };

    // ASIO live monitoring: snapshot taken at driver load, compared each frame
    struct AsioSnapshot { double sr{0}; int buf{0}; int bmin{0}; int bpref{0}; };
    AsioSnapshot m_asioSnap;          // values when driver was loaded
    AsioSnapshot m_asioLive;          // values re-queried after CP opened
    bool  m_asioCpOpened    { false }; // CP was opened at least once this session
    bool  m_asioNeedsReload { false }; // live != snap → show reload warning
    bool  m_showSettingsWindow { false };
    int   m_selectedNodeId     { -1 };
    int   m_pendingDeleteNode  { -1 };
    int   m_pendingDeleteEdge  { -1 };
    char  m_savePathBuf[260]   {};
    char  m_loadPathBuf[260]   {};
    bool  m_engineStarted      { false };

    // Track which nodes have already been positioned (fix drag bug)
    std::set<int> m_positionedNodes;

    // For right-click context menu - track which node was clicked
    int           m_rightClickNode { -1 };
    int           m_rightClickEdge { -1 };  // for link right-click context menu

    // Graph zoom / pan
    float         m_graphZoom      { 1.0f };   // 0.3 .. 2.5
    ImVec2        m_graphPan       { 0.0f, 0.0f };
    bool          m_applyZoom      { false };  // pending zoom change

    // Scrolling device names: map from device ID → scroll offset (chars)
    std::unordered_map<std::string,float> m_nameScroll;

    std::vector<AsioDeviceInfo> m_asioDrivers;
    int m_selectedAsioDriver { -1 };

    VolumeMixer m_volumeMixer;
    bool        m_showVolumeMixer { false };

    static ImVec4      nodeColor(DeviceType t);
    static const char* deviceTypeIcon(DeviceType t);
};


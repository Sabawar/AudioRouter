#pragma once
#include "RoutingGraph.h"
#include "WasapiManager.h"
#include "AsioManager.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <unordered_map>

// ============================================================================
//  AudioEngine  –  translates the RoutingGraph into live audio streams
//  Responsibilities:
//    • Start/stop WASAPI streams according to graph nodes
//    • Run the mix thread: pull from sources, accumulate, push to sinks
//    • Route ASIO ↔ WASAPI when both exist in the graph
//    • Keep RoutingGraph peak meters up-to-date
// ============================================================================
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool init(RoutingGraph* graph);
    void shutdown();

    // Call after graph changes to resync active streams
    void syncToGraph();

    // Start/stop all routing
    bool start();
    void stop();
    bool isRunning() const { return m_running; }

    // ASIO device management (one driver at a time)
    std::vector<AsioDeviceInfo> enumerateAsioDevices();
    bool  loadAsioDriver(const AsioDeviceInfo& dev, HWND hwnd);
    void  registerAsioStreams();   // call after inputNodeId/outputNodeId are set
    void  unloadAsioDriver();
    bool  isAsioLoaded()  const { return m_asio.isLoaded(); }
    AsioManager& asio()         { return m_asio; }

    WasapiManager& wasapi()     { return m_wasapi; }

    // Convenience: add device node into graph from a discovered device
    int addWasapiNode(const WasapiDeviceInfo& dev);

    // Mix sample rate used for resampling  
    int mixSampleRate { 48000 };
    // 480 frames = 10ms at 48kHz. MUST match mix thread sleep interval.
    // Also equals RNNoise frame size exactly.
    int mixBufferSize { 480 };

private:
    void mixThread();
    void processMixCycle();

    // For each output node, collect all connected input nodes and mix
    void mixToSink(int sinkNodeId);

    RoutingGraph*  m_graph   { nullptr };
    WasapiManager  m_wasapi;
    AsioManager    m_asio;

    std::thread         m_mixThread;
    std::atomic<bool>   m_running  { false };
    std::atomic<bool>   m_stopFlag { false };

    // Temp buffers
    std::vector<float>  m_srcBuf;
    std::vector<float>  m_mixBuf;
    std::vector<float>  m_monoTmp;  // mono upmix scratch buffer

    // ASIO → mix-engine resampler state (linear interp, stereo)
    std::vector<float>  m_asioResamplePrev;  // last ASIO frame (stereo)
    double              m_asioResamplePos   { 0.0 };
    double              m_asioResampleRatio { 1.0 }; // asioSR / mixSR
    std::vector<float>  m_asioResampleBuf;           // staging buffer

    // Per Mixer-node accumulated output buffer (filled in first pass)
    std::unordered_map<int, std::vector<float>> m_mixerBufs;

    // Per-node gain (copied from RoutingGraph on syncToGraph)
    std::unordered_map<int, float> m_nodeGains;

    // Read audio from any source node type (capture, loopback, or Mixer)
    size_t readFromSource(int srcNodeId, int frames, int ch);
};

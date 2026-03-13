#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>    // std::clamp
#include <cmath>        // std::log10f, std::pow
#include "RoutingGraph.h"
#include "AudioEngine.h"
#include "WasapiManager.h"
#include "imgui.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>

// ============================================================================
//  VolumeMixer
//
//  Standalone ImGui window with:
//    • Vertical faders for every node that has incoming audio
//    • System endpoint volume (IAudioEndpointVolume) per device
//    • Software gain (RoutingGraph::gainDb) per node
//    • Mute button per channel
//    • Stereo peak meter with hold, RMS, clip indicator
//    • Per-app session mixer for the default render device
// ============================================================================

// Per-channel metering state (peak hold + RMS accumulation)
struct MeterState {
    float peakL      { 0.0f };
    float peakR      { 0.0f };
    float holdL      { 0.0f };
    float holdR      { 0.0f };
    float rmsL       { 0.0f };
    float rmsR       { 0.0f };
    bool  clipL      { false };
    bool  clipR      { false };
    std::chrono::steady_clock::time_point holdTimeL;
    std::chrono::steady_clock::time_point holdTimeR;

    void update(float newPeakL, float newPeakR);
    void resetClip() { clipL = clipR = false; }
};

class VolumeMixer {
public:
    VolumeMixer() = default;

    void init(RoutingGraph* graph, AudioEngine* engine);

    // Call every frame; opens/closes a floating window
    void renderWindow(bool* p_open);

private:
    // ── Drawing sections ────────────────────────────────────────────────────
    void drawNodeStrip(RoutingNode& node);
    void drawAppSessionSection();
    void drawVerticalFader(const char* id, float* valueNorm,
                           float width, float height);
    void drawStereoMeter(MeterState& m, float width, float height);
    void drawDbLabels(float x, float y, float height, float minDb, float maxDb);

    // ── Helpers ─────────────────────────────────────────────────────────────
    std::wstring nodeDeviceId(const RoutingNode& n) const;
    bool isInputNode(const RoutingNode& n) const;
    static float linearToDb(float v) {
        return (v > 0.00001f) ? 20.0f * std::log10f(v) : -96.0f;
    }
    static float dbToLinear(float db) {
        return std::pow(10.0f, db / 20.0f);
    }
    // Map dBFS [-96, 0] → [0, 1] for meter display
    static float dbToMeterPos(float db, float minDb = -60.0f) {
        return std::clamp((db - minDb) / (0.0f - minDb), 0.0f, 1.0f);
    }

    RoutingGraph* m_graph  { nullptr };
    AudioEngine*  m_engine { nullptr };

    // Per-node meter state
    std::unordered_map<int, MeterState> m_meters;

    // Cached app sessions (refreshed every ~500ms)
    std::vector<WasapiManager::AppSession> m_sessions;
    std::chrono::steady_clock::time_point  m_sessionRefreshTime;
    bool m_showAppSessions { true };

    // Which render device to query sessions from
    // (defaults to Windows default playback)
    std::wstring m_sessionDeviceId;

    // UI state
    bool  m_showRms     { true };
    bool  m_showHold    { true };
    float m_meterMinDb  { -60.0f };
};

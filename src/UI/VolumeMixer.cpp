#include "VolumeMixer.h"
#include "imgui_internal.h"
#include "Lang.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// Safe wstring → UTF-8
static std::string wToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ──────────────────────────────────────────────────────────────────────────────
//  MeterState
// ──────────────────────────────────────────────────────────────────────────────
static constexpr float kHoldSec   = 1.5f;   // peak hold duration
static constexpr float kDecayRate = 0.94f;  // per-frame decay multiplier

void MeterState::update(float newPeakL, float newPeakR) {
    auto now = std::chrono::steady_clock::now();

    // Clip detection
    if (newPeakL >= 0.999f) { clipL = true; }
    if (newPeakR >= 0.999f) { clipR = true; }

    // Smooth decay for bar
    peakL = std::max(newPeakL, peakL * kDecayRate);
    peakR = std::max(newPeakR, peakR * kDecayRate);

    // Hold logic
    if (newPeakL >= holdL) {
        holdL = newPeakL;
        holdTimeL = now;
    } else {
        float elapsed = std::chrono::duration<float>(now - holdTimeL).count();
        if (elapsed > kHoldSec)
            holdL = std::max(0.0f, holdL - 0.003f);
    }
    if (newPeakR >= holdR) {
        holdR = newPeakR;
        holdTimeR = now;
    } else {
        float elapsed = std::chrono::duration<float>(now - holdTimeR).count();
        if (elapsed > kHoldSec)
            holdR = std::max(0.0f, holdR - 0.003f);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
//  Init
// ──────────────────────────────────────────────────────────────────────────────
void VolumeMixer::init(RoutingGraph* graph, AudioEngine* engine) {
    m_graph  = graph;
    m_engine = engine;
    m_sessionDeviceId = engine->wasapi().defaultPlaybackId();
}

// ──────────────────────────────────────────────────────────────────────────────
//  Main window
// ──────────────────────────────────────────────────────────────────────────────
void VolumeMixer::renderWindow(bool* p_open) {
    if (!*p_open) return;

    ImGui::SetNextWindowSizeConstraints({320, 300}, {2400, 900});
    ImGui::SetNextWindowSize({900, 560}, ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 8});
    if (!ImGui::Begin(T(Str::MIXER_TITLE), p_open,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar();

    // ── Toolbar ──────────────────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::Checkbox(T(Str::MIXER_RMS), &m_showRms);
    ImGui::SameLine();
    ImGui::Checkbox(T(Str::MIXER_HOLD), &m_showHold);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat(T(Str::MIXER_MIN_DB), &m_meterMinDb, -96.0f, -20.0f, "%.0f dB");
    ImGui::SameLine();
    ImGui::Checkbox(T(Str::MIXER_APP_SESSIONS), &m_showAppSessions);
    ImGui::SameLine();
    if (ImGui::SmallButton(T(Str::MIXER_RESET_CLIPS))) {
        for (auto& kv : m_meters) kv.second.resetClip();
    }
    ImGui::PopStyleVar();

    ImGui::Separator();

    // ── Scrollable strip area ─────────────────────────────────────────────────
    ImGui::BeginChild("##mixerStrips", {0, m_showAppSessions ? -180.0f : -4.0f},
                       false, ImGuiWindowFlags_HorizontalScrollbar);

    {
        std::lock_guard<std::mutex> lk(m_graph->mutex);
        for (auto& node : m_graph->nodes()) {
            // Show all nodes that produce or carry audio
            drawNodeStrip(node);
            ImGui::SameLine();
        }
    }

    // Dummy to ensure horizontal scroll area is computed
    ImGui::Dummy({0, 0});
    ImGui::EndChild();

    // ── Per-app session mixer ─────────────────────────────────────────────────
    if (m_showAppSessions) {
        ImGui::Separator();
        drawAppSessionSection();
    }

    ImGui::End();
}

// ──────────────────────────────────────────────────────────────────────────────
//  One channel strip for a RoutingNode
// ──────────────────────────────────────────────────────────────────────────────
void VolumeMixer::drawNodeStrip(RoutingNode& node) {
    const float STRIP_W      = 72.0f;
    const float FADER_H      = 160.0f;
    const float METER_H      = 160.0f;
    const float METER_W_EACH = 10.0f;
    const float STRIP_TOTAL  = STRIP_W + METER_W_EACH * 2 + 6.0f;

    ImGui::PushID(node.id);
    ImGui::BeginGroup();

    auto* dl    = ImGui::GetWindowDrawList();
    ImVec2 base = ImGui::GetCursorScreenPos();

    // ── Background ────────────────────────────────────────────────────────────
    ImVec4 bgColF = {0.14f, 0.14f, 0.16f, 1.0f};
    dl->AddRectFilled(base,
                      {base.x + STRIP_TOTAL, base.y + FADER_H + 90},
                      ImGui::ColorConvertFloat4ToU32(bgColF), 6.0f);

    // ── Node name (truncated) ─────────────────────────────────────────────────
    {
        std::string label = node.name;
        if (label.size() > 10) label = label.substr(0, 9) + "..";
        ImGui::SetNextItemWidth(STRIP_TOTAL);
        ImGui::TextDisabled("%s", label.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nID: %d", node.name.c_str(), node.id);
    }

    // ── Stereo peak meter ─────────────────────────────────────────────────────
    auto& meter = m_meters[node.id];
    meter.update(node.peakL, node.peakR);

    ImGui::SetCursorScreenPos({base.x + 2, base.y + 18});
    drawStereoMeter(meter, METER_W_EACH * 2 + 2, METER_H);

    // ── Vertical software fader (gainDb) ─────────────────────────────────────
    // Map gainDb [-40, +12] → [0, 1]
    float normGain = (node.gainDb + 40.0f) / 52.0f;
    normGain = std::clamp(normGain, 0.0f, 1.0f);
    ImGui::SetCursorScreenPos({base.x + METER_W_EACH * 2 + 6, base.y + 18});
    drawVerticalFader(("##swfader" + std::to_string(node.id)).c_str(),
                      &normGain, STRIP_W - 4, FADER_H);
    node.gainDb = normGain * 52.0f - 40.0f;

    // ── dB readout ────────────────────────────────────────────────────────────
    ImGui::SetCursorScreenPos({base.x + 2, base.y + FADER_H + 20});
    ImGui::SetNextItemWidth(STRIP_TOTAL - 4);
    float tmpDb = node.gainDb;
    if (ImGui::DragFloat(("##dbval" + std::to_string(node.id)).c_str(),
                         &tmpDb, 0.1f, -40.0f, 12.0f, "%.1f dB"))
        node.gainDb = tmpDb;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        node.gainDb = 0.0f;  // right-click = reset to 0dB

    // ── System endpoint volume (if device has an ID) ──────────────────────────
    std::wstring devId = nodeDeviceId(node);
    if (!devId.empty()) {
        float sysVol  = m_engine->wasapi().getEndpointVolume(devId);
        bool  sysMute = m_engine->wasapi().getEndpointMute(devId);

        ImGui::SetCursorScreenPos({base.x + 2, base.y + FADER_H + 40});
        ImGui::TextDisabled("%s", T(Str::MIXER_SYS));
        ImGui::SetCursorScreenPos({base.x + 2, base.y + FADER_H + 54});
        ImGui::SetNextItemWidth(STRIP_TOTAL - 4);
        if (ImGui::SliderFloat(("##sysvol" + std::to_string(node.id)).c_str(),
                               &sysVol, 0.0f, 1.0f, "%.0f%%",
                               ImGuiSliderFlags_None))
        {
            m_engine->wasapi().setEndpointVolume(devId, sysVol);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("System volume: %.0f%%\n(Right-click = 100%%)", sysVol * 100.0f);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            m_engine->wasapi().setEndpointVolume(devId, 1.0f);

        // Mute button
        ImGui::SetCursorScreenPos({base.x + 2, base.y + FADER_H + 74});
        ImVec4 muteBtnCol = sysMute
            ? ImVec4{0.85f,0.15f,0.15f,1.0f}
            : ImVec4{0.25f,0.25f,0.30f,1.0f};
        ImGui::PushStyleColor(ImGuiCol_Button, muteBtnCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            sysMute ? ImVec4{1.0f,0.25f,0.25f,1.0f} : ImVec4{0.35f,0.35f,0.40f,1.0f});
        if (ImGui::Button(sysMute ? " MUTED " : "  MUTE ",
                          {STRIP_TOTAL - 4, 0}))
        {
            m_engine->wasapi().setEndpointMute(devId, !sysMute);
        }
        ImGui::PopStyleColor(2);
    }

    // ── Software enable toggle ────────────────────────────────────────────────
    ImGui::SetCursorScreenPos({base.x + 2, base.y + FADER_H + 92});
    ImGui::PushStyleColor(ImGuiCol_CheckMark, {0.3f,0.9f,0.4f,1.0f});
    ImGui::Checkbox(("##en" + std::to_string(node.id)).c_str(), &node.enabled);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable routing for this node");

    ImGui::EndGroup();
    ImGui::PopID();

    // Vertical separator between strips
    float sx = ImGui::GetCursorScreenPos().x - 2;
    float sy = base.y;
    dl->AddLine({sx, sy}, {sx, sy + FADER_H + 100}, IM_COL32(60,60,65,180));
}

// ──────────────────────────────────────────────────────────────────────────────
//  Per-app session section (bottom of mixer)
// ──────────────────────────────────────────────────────────────────────────────
void VolumeMixer::drawAppSessionSection() {
    ImGui::TextColored({0.7f,0.7f,0.3f,1.0f}, "%s", T(Str::MIXER_APP_TITLE));
    ImGui::SameLine(0, 20);
    if (ImGui::SmallButton(T(Str::MIXER_REFRESH))) m_sessionRefreshTime = {};

    // Refresh sessions every 500ms
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - m_sessionRefreshTime).count();
    if (elapsed > 0.5f) {
        std::wstring id = m_engine->wasapi().defaultPlaybackId();
        if (!id.empty()) {
            m_sessions = m_engine->wasapi().getAppSessions(id);
            m_sessionDeviceId = id;
        }
        m_sessionRefreshTime = now;
    }

    ImGui::BeginChild("##appSessions", {0, 0}, false,
                       ImGuiWindowFlags_HorizontalScrollbar);

    if (m_sessions.empty()) {
        ImGui::TextDisabled("%s", T(Str::MIXER_NO_SESSIONS));
    }

    for (auto& s : m_sessions) {
        ImGui::PushID(s.id.c_str());

        // App name (narrow)
        std::string name = wToUtf8(s.name);
        if (name.size() > 22) name = name.substr(0, 20) + "..";
        ImGui::SetNextItemWidth(160);
        ImGui::Text("%-22s", name.c_str());
        if (ImGui::IsItemHovered()) {
            std::string fullName = wToUtf8(s.name);
            ImGui::SetTooltip("%s", fullName.c_str());
        }

        ImGui::SameLine(170);

        // Horizontal volume slider
        ImGui::SetNextItemWidth(220);
        float vol = s.volume;
        if (ImGui::SliderFloat("##vol", &vol, 0.0f, 1.0f, "%.0f%%")) {
            s.volume = vol;
            m_engine->wasapi().setAppSessionVolume(m_sessionDeviceId, s.id, vol);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            s.volume = 1.0f;
            m_engine->wasapi().setAppSessionVolume(m_sessionDeviceId, s.id, 1.0f);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Volume: %.0f%%\n(Right-click = 100%%)", vol * 100.0f);

        ImGui::SameLine();

        // Mute button
        ImGui::PushStyleColor(ImGuiCol_Button,
            s.muted ? ImVec4{0.7f,0.1f,0.1f,1.0f} : ImVec4{0.25f,0.25f,0.28f,1.0f});
        if (ImGui::SmallButton(s.muted ? T(Str::MIXER_MUTED) : T(Str::MIXER_MUTE))) {
            s.muted = !s.muted;
            m_engine->wasapi().setAppSessionMute(m_sessionDeviceId, s.id, s.muted);
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        // Mini peak bar
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float barW = 80.0f, barH = 10.0f;
        auto* dl = ImGui::GetWindowDrawList();
        // Background
        dl->AddRectFilled(pos, {pos.x + barW, pos.y + barH}, IM_COL32(30,30,30,220));
        // L channel
        float fillL = std::clamp(s.peakL, 0.0f, 1.0f) * (barW * 0.5f - 1);
        if (fillL > 0)
            dl->AddRectFilled(pos, {pos.x + fillL, pos.y + barH * 0.5f - 1},
                              s.peakL > 0.9f ? IM_COL32(220,50,50,255) :
                              s.peakL > 0.6f ? IM_COL32(200,160,30,255) :
                                               IM_COL32(50,180,70,220));
        // R channel
        float fillR = std::clamp(s.peakR, 0.0f, 1.0f) * (barW * 0.5f - 1);
        if (fillR > 0)
            dl->AddRectFilled({pos.x, pos.y + barH * 0.5f + 1},
                              {pos.x + fillR, pos.y + barH},
                              s.peakR > 0.9f ? IM_COL32(220,50,50,255) :
                              s.peakR > 0.6f ? IM_COL32(200,160,30,255) :
                                               IM_COL32(50,180,70,220));
        dl->AddRect(pos, {pos.x + barW, pos.y + barH}, IM_COL32(80,80,80,180));
        ImGui::Dummy({barW, barH});

        ImGui::PopID();
    }

    ImGui::EndChild();
}

// ──────────────────────────────────────────────────────────────────────────────
//  Custom vertical fader (drawn with ImGui DrawList)
// ──────────────────────────────────────────────────────────────────────────────
void VolumeMixer::drawVerticalFader(const char* id, float* valueNorm,
                                    float width, float height)
{
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGuiID gid = ImGui::GetID(id);
    ImRect  bb(pos, {pos.x + width, pos.y + height});

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return;

    // ButtonBehavior properly handles click, drag, hover state tracking
    bool hovered = false, held = false;
    ImGui::ButtonBehavior(bb, gid, &hovered, &held,
                          ImGuiButtonFlags_PressedOnClick);

    // Drag to change value (smooth drag)
    if (held && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        float delta = -ImGui::GetIO().MouseDelta.y / height;
        *valueNorm = std::clamp(*valueNorm + delta, 0.0f, 1.0f);
        ImGui::SetActiveID(gid, ImGui::GetCurrentWindow());
    }
    // Scroll wheel
    if (hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            *valueNorm = std::clamp(*valueNorm + wheel * 0.025f, 0.0f, 1.0f);
    }
    // Double-click = unity (0dB position = 40/52 ≈ 0.769)
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        *valueNorm = 40.0f / 52.0f;

    auto* dl = ImGui::GetWindowDrawList();
    // Track
    float cx = pos.x + width * 0.5f;
    dl->AddRectFilled({cx-3, pos.y + 4}, {cx+3, pos.y + height - 4},
                      IM_COL32(30,30,30,240), 3.0f);

    // Tick marks at -40, -20, -12, -6, 0, +6, +12 dB
    const float ticks[] = {-40,-30,-20,-12,-6,0,6,12};
    for (float db : ticks) {
        float norm = (db + 40.0f) / 52.0f;
        float ty   = pos.y + height - norm * height;
        ImU32 tcol = (db == 0.0f) ? IM_COL32(200,200,100,200) : IM_COL32(80,80,80,180);
        dl->AddLine({cx-8, ty}, {cx+8, ty}, tcol, 1.0f);
    }

    // Unity (0dB) mark prominent
    float unity = pos.y + height - (40.0f / 52.0f) * height;
    dl->AddLine({cx-12, unity}, {cx+12, unity}, IM_COL32(220,200,60,220), 1.5f);

    // Filled portion below knob
    float knobY = pos.y + height - (*valueNorm) * height;
    float fillFrac = *valueNorm;
    ImU32 fillCol = fillFrac > (40.0f+12.0f)/52.0f
                    ? IM_COL32(220,80,50,200)    // above unity+12 → red
                    : fillFrac > 40.0f/52.0f
                    ? IM_COL32(200,160,30,200)   // above unity → yellow
                    : IM_COL32(50,160,90,200);   // normal → green
    if (*valueNorm > 0.001f)
        dl->AddRectFilled({cx-3, knobY}, {cx+3, pos.y + height - 4}, fillCol, 2.0f);

    // Knob
    float kw = width * 0.85f, kh = 14.0f;
    ImVec2 kpos = {pos.x + (width-kw)*0.5f, knobY - kh*0.5f};
    ImU32 knobFill = hovered || held
                     ? IM_COL32(120,155,210,255)
                     : IM_COL32(85,110,160,255);
    dl->AddRectFilled(kpos, {kpos.x+kw, kpos.y+kh}, knobFill, 3.0f);
    dl->AddRect      (kpos, {kpos.x+kw, kpos.y+kh}, IM_COL32(180,200,240,200), 3.0f);
    // Center line on knob
    float km = kpos.y + kh * 0.5f;
    dl->AddLine({kpos.x+4, km}, {kpos.x+kw-4, km}, IM_COL32(220,230,255,200), 1.5f);
}

// ──────────────────────────────────────────────────────────────────────────────
//  Stereo vertical meter
// ──────────────────────────────────────────────────────────────────────────────
void VolumeMixer::drawStereoMeter(MeterState& m, float width, float height) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    auto*  dl  = ImGui::GetWindowDrawList();
    float  ch  = width * 0.5f - 1.0f;   // per-channel width

    auto drawChannel = [&](float x, float peak, float hold, float rms,
                           bool clip)
    {
        // Background
        dl->AddRectFilled({x, pos.y}, {x+ch, pos.y+height},
                          IM_COL32(20,20,22,240), 2.0f);

        // Gradient fill — green / yellow / red zones
        float fillH = std::clamp(peak, 0.0f, 1.0f) * height;
        float top   = pos.y + height - fillH;

        // Green portion (bottom → -6dBFS)
        float thresh6db  = dbToMeterPos(-6.0f,  m_meterMinDb) * height;
        float thresh12db = dbToMeterPos(-12.0f, m_meterMinDb) * height;

        if (fillH > 0) {
            if (fillH <= thresh12db) {
                dl->AddRectFilled({x, top}, {x+ch, pos.y+height},
                                  IM_COL32(40,190,70,230));
            } else if (fillH <= thresh6db) {
                dl->AddRectFilled({x, pos.y+height-thresh12db},
                                  {x+ch, pos.y+height}, IM_COL32(40,190,70,230));
                dl->AddRectFilled({x, top},
                                  {x+ch, pos.y+height-thresh12db},
                                  IM_COL32(190,170,30,230));
            } else {
                dl->AddRectFilled({x, pos.y+height-thresh12db},
                                  {x+ch, pos.y+height}, IM_COL32(40,190,70,230));
                dl->AddRectFilled({x, pos.y+height-thresh6db},
                                  {x+ch, pos.y+height-thresh12db},
                                  IM_COL32(190,170,30,230));
                dl->AddRectFilled({x, top},
                                  {x+ch, pos.y+height-thresh6db},
                                  clip ? IM_COL32(255,30,30,255) : IM_COL32(220,60,40,230));
            }
        }

        // RMS line
        if (m_showRms && rms > 0.001f) {
            float ry = pos.y + height - std::clamp(rms, 0.0f, 1.0f) * height;
            dl->AddLine({x, ry}, {x+ch, ry}, IM_COL32(100,220,255,180), 1.0f);
        }

        // Hold line
        if (m_showHold && hold > 0.001f) {
            float hy = pos.y + height - std::clamp(hold, 0.0f, 1.0f) * height;
            dl->AddLine({x, hy}, {x+ch, hy},
                        hold >= 0.999f ? IM_COL32(255,80,80,255)
                                       : IM_COL32(240,240,100,220), 2.0f);
        }

        // Clip indicator (top 4px)
        if (clip)
            dl->AddRectFilled({x, pos.y}, {x+ch, pos.y+4}, IM_COL32(255,30,30,255));

        // Tick lines
        for (float db : {-6.0f,-12.0f,-18.0f,-24.0f,-36.0f,-48.0f}) {
            float norm = dbToMeterPos(db, m_meterMinDb);
            if (norm < 0 || norm > 1) continue;
            float ty = pos.y + height - norm * height;
            dl->AddLine({x, ty}, {x+ch, ty}, IM_COL32(50,55,55,180), 1.0f);
        }

        // Border
        dl->AddRect({x, pos.y}, {x+ch, pos.y+height}, IM_COL32(70,70,75,200), 2.0f);
    };

    drawChannel(pos.x,      m.peakL, m.holdL, m.rmsL, m.clipL);
    drawChannel(pos.x+ch+1, m.peakR, m.holdR, m.rmsR, m.clipR);

    ImGui::Dummy({width, height});
}

// ──────────────────────────────────────────────────────────────────────────────
//  Helpers
// ──────────────────────────────────────────────────────────────────────────────
std::wstring VolumeMixer::nodeDeviceId(const RoutingNode& n) const {
    // deviceId is stored as std::string (narrow, originally from wstring)
    return std::wstring(n.deviceId.begin(), n.deviceId.end());
}

bool VolumeMixer::isInputNode(const RoutingNode& n) const {
    return n.type == DeviceType::WasapiCapture  ||
           n.type == DeviceType::WasapiLoopback ||
           n.type == DeviceType::AsioInput;
}

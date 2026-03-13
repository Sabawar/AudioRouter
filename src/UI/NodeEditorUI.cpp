#include "NodeEditorUI.h"
#include "Logger.h"
#include "Lang.h"
#include "Updater.h"
#include "AppSettings.h"
#include "NoiseFilter.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imnodes.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// defined in main.cpp
extern std::string g_settingsPath;

// ──────────────────────────────────────────────────────────────────────────────
//  Helpers
// ──────────────────────────────────────────────────────────────────────────────
static std::string truncate(const std::string& s, size_t n) {
    return s.size() > n ? s.substr(0, n-2) + ".." : s;
}
static float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

// Safe wstring → UTF-8 (handles Cyrillic, special chars, etc.)
static std::string wToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

ImVec4 NodeEditorUI::nodeColor(DeviceType t) {
    switch (t) {
    // Green — real WASAPI audio devices
    case DeviceType::WasapiCapture:  return {0.12f,0.55f,0.20f,1.0f};
    case DeviceType::WasapiLoopback: return {0.10f,0.50f,0.40f,1.0f};
    case DeviceType::WasapiRender:   return {0.08f,0.48f,0.15f,1.0f};
    // Red — ASIO devices
    case DeviceType::AsioInput:      return {0.65f,0.10f,0.10f,1.0f};
    case DeviceType::AsioOutput:     return {0.55f,0.08f,0.08f,1.0f};
    // Purple — virtual devices / mixer
    case DeviceType::VirtualSink:    return {0.48f,0.12f,0.70f,1.0f};
    case DeviceType::Mixer:          return {0.40f,0.10f,0.58f,1.0f};
    }
    return {0.4f,0.4f,0.4f,1.0f};
}
const char* NodeEditorUI::deviceTypeIcon(DeviceType t) {
    switch (t) {
    case DeviceType::WasapiCapture:  return T(Str::DEV_MIC);
    case DeviceType::WasapiLoopback: return T(Str::DEV_LOOPBACK);
    case DeviceType::WasapiRender:   return T(Str::DEV_SPEAKER);
    case DeviceType::AsioInput:      return T(Str::DEV_ASIO_IN);
    case DeviceType::AsioOutput:     return T(Str::DEV_ASIO_OUT);
    case DeviceType::VirtualSink:    return T(Str::DEV_VIRTUAL);
    case DeviceType::Mixer:          return T(Str::DEV_MIXER);
    }
    return "";
}

// ──────────────────────────────────────────────────────────────────────────────
//  Init / Shutdown
// ──────────────────────────────────────────────────────────────────────────────
NodeEditorUI::NodeEditorUI()  = default;
NodeEditorUI::~NodeEditorUI() { shutdown(); }

void NodeEditorUI::init(RoutingGraph* graph, AudioEngine* engine) {
    m_graph  = graph;
    m_engine = engine;

    ImNodes::CreateContext();
    m_imnodesCtx = ImNodes::GetCurrentContext();

    // Style
    ImNodes::GetStyle().NodePadding       = {10, 8};
    ImNodes::GetStyle().NodeCornerRounding = 6.0f;
    ImNodes::GetStyle().LinkThickness      = 2.5f;
    ImNodes::GetStyle().PinCircleRadius    = 5.0f;

    ImNodes::StyleColorsDark();
    ImNodes::GetStyle().Colors[ImNodesCol_NodeBackground]         = IM_COL32(40,40,40,230);
    ImNodes::GetStyle().Colors[ImNodesCol_NodeBackgroundHovered]  = IM_COL32(55,55,55,230);
    ImNodes::GetStyle().Colors[ImNodesCol_NodeBackgroundSelected] = IM_COL32(65,65,95,230);
    ImNodes::GetStyle().Colors[ImNodesCol_Link]                   = IM_COL32(220,160,30,200);
    ImNodes::GetStyle().Colors[ImNodesCol_LinkHovered]            = IM_COL32(255,200,60,255);
    ImNodes::GetStyle().Colors[ImNodesCol_LinkSelected]           = IM_COL32(255,220,80,255);

    // Refresh device lists
    m_asioDrivers = m_engine->enumerateAsioDevices();
    m_volumeMixer.init(graph, engine);

    // Auto-check for updates in background (silent)
    if (UPDATER_CHECK_ON_START)
        Updater::get().startCheck();
}

void NodeEditorUI::shutdown() {
    if (m_imnodesCtx) {
        ImNodes::DestroyContext();
        m_imnodesCtx = nullptr;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
//  Main frame
// ──────────────────────────────────────────────────────────────────────────────
void NodeEditorUI::renderFrame(HWND hwnd) {
    // Full-window layout
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0,0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_MenuBar  |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    drawMenuBar(hwnd);

    // 3-column layout: devices | node editor | properties
    float panelW   = 230.0f;
    float centerW  = io.DisplaySize.x - panelW * 2.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0,0});
    ImGui::Columns(3, "mainLayout", false);
    ImGui::SetColumnWidth(0, panelW);
    ImGui::SetColumnWidth(1, centerW);
    ImGui::SetColumnWidth(2, panelW);
    ImGui::PopStyleVar();

    // Content height = available area minus status bar (separator + one text line)
    float statusBarH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 2.0f;
    float childH     = ImGui::GetContentRegionAvail().y - statusBarH;
    if (childH < 50.0f) childH = 50.0f;

    // ─── Left: device panel ─────────────────────────────────────────────────
    ImGui::BeginChild("##devpanel", {0, childH}, true);
    drawDevicePanel();
    ImGui::EndChild();

    ImGui::NextColumn();

    // ─── Center: node editor ────────────────────────────────────────────────
    ImGui::BeginChild("##nodeeditor", {0, childH}, true);
    drawNodeEditor();
    ImGui::EndChild();

    ImGui::NextColumn();

    // ─── Right: properties ──────────────────────────────────────────────────
    ImGui::BeginChild("##propspanel", {0, childH}, true);
    drawPropertiesPanel();
    ImGui::EndChild();

    ImGui::Columns(1);

    // ─── Bottom status bar ───────────────────────────────────────────────────
    drawStatusBar();

    ImGui::End();

    // ─── Volume Mixer window ──────────────────────────────────────────────────
    m_volumeMixer.renderWindow(&m_showVolumeMixer);
    drawSettingsWindow();

    // Handle deferred deletions
    if (m_pendingDeleteNode >= 0) {
        if (m_engineStarted) m_engine->stop();
        m_graph->removeNode(m_pendingDeleteNode);
        m_positionedNodes.erase(m_pendingDeleteNode);  // allow re-positioning if re-added
        if (m_engineStarted) m_engine->start();
        m_pendingDeleteNode = -1;
        m_selectedNodeId    = -1;
    }
    if (m_pendingDeleteEdge >= 0) {
        m_graph->removeEdge(m_pendingDeleteEdge);
        m_pendingDeleteEdge = -1;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
//  Menu bar
// ──────────────────────────────────────────────────────────────────────────────
void NodeEditorUI::drawMenuBar(HWND hwnd) {
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu(T(Str::MENU_FILE))) {
        if (ImGui::MenuItem(T(Str::MENU_FILE_NEW)))   { m_graph->clear(); }
        if (ImGui::MenuItem(T(Str::MENU_FILE_SAVE))) {
            if (m_graph->saveToFile("AudioRouter.patch"))
                MessageBoxA(hwnd, T(Str::MSG_SAVED_TO), T(Str::MSG_SAVED),
                            MB_OK|MB_ICONINFORMATION);
        }
        if (ImGui::MenuItem(T(Str::MENU_FILE_LOAD))) {
            m_graph->loadFromFile("AudioRouter.patch");
            if (m_engineStarted) { m_engine->stop(); m_engine->start(); }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T(Str::MENU_FILE_EXIT))) PostQuitMessage(0);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T(Str::MENU_ENGINE))) {
        if (!m_engineStarted) {
            if (ImGui::MenuItem(T(Str::MENU_ENGINE_START))) {
                m_engine->start();
                m_engineStarted = true;
            }
        } else {
            if (ImGui::MenuItem(T(Str::MENU_ENGINE_STOP))) {
                m_engine->stop();
                m_engineStarted = false;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T(Str::MENU_ENGINE_REFRESH))) {
            m_engine->wasapi().refreshDeviceList();
            m_asioDrivers = m_engine->enumerateAsioDevices();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T(Str::MENU_ASIO))) {
        ImGui::MenuItem(T(Str::MENU_ASIO_SETTINGS), nullptr, &m_showAsioPanel);
        if (m_engine->isAsioLoaded()) {
            if (ImGui::MenuItem(T(Str::MENU_ASIO_PANEL)))
                m_engine->asio().showControlPanel();
            if (ImGui::MenuItem(T(Str::MENU_ASIO_UNLOAD)))
                m_engine->unloadAsioDriver();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T(Str::MENU_ADD))) {
        if (ImGui::MenuItem(T(Str::MENU_ADD_MIXER))) {
            int id = m_graph->addNode("Mixer", "__mixer__", DeviceType::Mixer);
            auto* n = m_graph->findNode(id);
            if (n) { n->posX = 400; n->posY = 200; }
        }
        ImGui::EndMenu();
    }

    if (ImGui::MenuItem(m_showVolumeMixer ? "  [X] Volume Mixer" : T(Str::MENU_VIEW_VOLUME)))
        m_showVolumeMixer = !m_showVolumeMixer;

    extern bool g_showLog;
    if (ImGui::MenuItem(g_showLog ? "  [X] Log" : T(Str::MENU_VIEW_LOG)))
        g_showLog = !g_showLog;

    // ── Настройки (язык, масштаб, автозапуск) ─────────────────────────────
    if (ImGui::BeginMenu(T(Str::SETTINGS_LANGUAGE))) {
        if (ImGui::MenuItem(T(Str::SETTINGS_RUSSIAN), nullptr, g_lang == Lang::RU))
            g_lang = Lang::RU;
        if (ImGui::MenuItem(T(Str::SETTINGS_ENGLISH), nullptr, g_lang == Lang::EN))
            g_lang = Lang::EN;
        ImGui::Separator();
        if (ImGui::MenuItem("Settings..."))
            m_showSettingsWindow = true;
        ImGui::EndMenu();
    }

    // ── Help menu ────────────────────────────────────────────────────────────
    bool ru0 = (g_lang == Lang::RU);
    // Show dot indicator on menu if update is available
    bool hasUpd = Updater::get().hasUpdate();
    const char* helpLabel = hasUpd
        ? (ru0 ? "Справка ●" : "Help ●")
        : (ru0 ? "Справка"   : "Help");
    if (hasUpd) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.4f,1.0f,0.4f,1.f});
    if (ImGui::BeginMenu(helpLabel)) {
        if (hasUpd) ImGui::PopStyleColor();

        // Update entry — shows version badge if available
        std::string updLabel;
        if (hasUpd) {
            auto info = Updater::get().latestInfo();
            updLabel = ru0
                ? ("↑ Обновление " + info.version + " доступно!")
                : ("↑ Update " + info.version + " available!");
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.4f,1.0f,0.4f,1.f});
            if (ImGui::MenuItem(updLabel.c_str())) m_showUpdater = true;
            ImGui::PopStyleColor();
        } else {
            if (ImGui::MenuItem(ru0 ? "Проверить обновление..."
                                    : "Check for updates..."))
            {
                m_showUpdater = true;
                Updater::get().startCheck();
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("AudioRouter v" UPDATER_CURRENT_VER);
        ImGui::EndMenu();
    } else {
        if (hasUpd) ImGui::PopStyleColor();
    }

    // Engine status + Start/Stop button — right-aligned
    float statusW = 300.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - statusW);

    // Start / Stop button
    bool ru = (g_lang == Lang::RU);
    if (m_engineStarted) {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.55f,0.15f,0.15f,1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.75f,0.20f,0.20f,1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.85f,0.25f,0.25f,1.0f});
        if (ImGui::SmallButton(ru ? "  СТОП  " : "  STOP  ")) {
            m_engine->stop();
            m_engineStarted = false;
        }
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.12f,0.45f,0.18f,1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.16f,0.60f,0.24f,1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.18f,0.68f,0.28f,1.0f});
        if (ImGui::SmallButton(ru ? "  СТАРТ  " : "  START  ")) {
            m_engine->start();
            m_engineStarted = true;
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::SameLine(0, 8);
    if (m_engineStarted)
        ImGui::TextColored({0.2f,0.9f,0.3f,1.0f}, "%s", T(Str::STATUS_ROUTING_ACTIVE));
    else
        ImGui::TextColored({0.9f,0.6f,0.1f,1.0f}, "%s", T(Str::STATUS_ROUTING_STOPPED));

    ImGui::EndMenuBar();

    // ── Update dialog ────────────────────────────────────────────────────────
    Updater::get().drawUI(m_showUpdater);

    // ASIO settings popup
    if (m_showAsioPanel) {
        ImGui::SetNextWindowSize({460,360}, ImGuiCond_FirstUseEver);
        ImGui::Begin(T(Str::MENU_ASIO_SETTINGS), &m_showAsioPanel);
        bool ru2 = (g_lang == Lang::RU);
        ImGui::Text("%s", T(Str::ASIO_DRIVERS));
        ImGui::Separator();
        for (int i = 0; i < (int)m_asioDrivers.size(); ++i) {
            auto& drv = m_asioDrivers[i];
            bool sel = (m_selectedAsioDriver == i);

            // Color: green = DLL found, yellow = 32-bit, red = DLL missing
            ImVec4 col = drv.dllPath.empty()
                ? ImVec4{0.9f,0.3f,0.3f,1.f}    // red - no DLL
                : drv.is32bit
                    ? ImVec4{0.9f,0.7f,0.1f,1.f} // yellow - 32-bit
                    : ImVec4{0.7f,1.0f,0.7f,1.f}; // green - ok

            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (ImGui::Selectable(drv.name.c_str(), sel))
                m_selectedAsioDriver = i;
            ImGui::PopStyleColor();

            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                if (!drv.dllPath.empty())
                    ImGui::Text("%S", drv.dllPath.c_str());
                else
                    ImGui::TextColored({1,0.4f,0.4f,1},
                        ru2 ? "DLL не найдена — драйвер не установлен"
                            : "DLL not found — driver not installed");
                if (drv.is32bit)
                    ImGui::TextColored({1,0.8f,0.2f,1},
                        ru2 ? "32-bit драйвер — может не загрузиться"
                            : "32-bit driver — may not load");
                ImGui::EndTooltip();
            }
        }
        if (m_asioDrivers.empty())
            ImGui::TextColored({0.8f,0.5f,0.2f,1.0f}, "%s", T(Str::ASIO_NO_DRIVERS));
        ImGui::Separator();
        if (m_selectedAsioDriver >= 0) {
            auto& d = m_asioDrivers[m_selectedAsioDriver];
            if (!m_engine->isAsioLoaded()) {
                if (ImGui::Button(T(Str::ASIO_LOAD))) {
                    if (m_engine->loadAsioDriver(d, hwnd)) {
                        auto& asio = m_engine->asio();
                        int inId  = m_graph->addNode(d.name + " IN",
                                                     "__asio_in__", DeviceType::AsioInput);
                        int outId = m_graph->addNode(d.name + " OUT",
                                                     "__asio_out__", DeviceType::AsioOutput);
                        asio.inputNodeId  = inId;
                        asio.outputNodeId = outId;
                        // Now that node IDs are known, register ring buffers
                        m_engine->registerAsioStreams();
                        m_showAsioPanel = false;
                    } else {
                        ImGui::TextColored({1,0.3f,0.3f,1}, "%s", T(Str::ASIO_LOAD_FAIL));
                    }
                }
            } else {
                auto& asio = m_engine->asio();
                bool ru2 = (g_lang == Lang::RU);

                // Driver name header
                ImGui::TextColored({0.4f,1.0f,0.5f,1.0f}, "%s", asio.driverName().c_str());
                ImGui::Separator();

                // ── Snapshot on first display ──────────────────────────────
                if (m_asioSnap.sr == 0) {
                    m_asioSnap = { asio.sampleRate(), asio.bufferSize(),
                                   asio.bufferSizeMin(), asio.bufferSizePref() };
                    m_asioLive = m_asioSnap;
                }

                // ── Channel info ───────────────────────────────────────────
                ImGui::Text("%s %d  %s %d",
                            T(Str::ASIO_INPUTS),  asio.numInputChannels(),
                            T(Str::ASIO_OUTPUTS), asio.numOutputChannels());

                // ── Live values ────────────────────────────────────────────
                m_asioLive = { asio.sampleRate(), asio.bufferSize(),
                               asio.bufferSizeMin(), asio.bufferSizePref() };

                // Detect changes vs snapshot (SR or buffer params)
                bool srChanged  = (m_asioLive.sr   != m_asioSnap.sr);
                bool bufChanged = (m_asioLive.bmin  != m_asioSnap.bmin ||
                                   m_asioLive.bpref != m_asioSnap.bpref);
                m_asioNeedsReload = (srChanged || bufChanged) && m_asioCpOpened;

                // Sample rate — highlighted if changed
                if (srChanged)
                    ImGui::TextColored({1.f,0.85f,0.1f,1.f},
                        "%.0f Hz  (!)", m_asioLive.sr);
                else
                    ImGui::Text("%.0f Hz", m_asioLive.sr);

                ImGui::Separator();

                // Buffer / latency table
                double sr    = m_asioLive.sr;
                int    bused = m_asioLive.buf;
                int    bmin  = m_asioLive.bmin;
                int    bpref = m_asioLive.bpref;
                float  latMs  = (sr>0)?(float)(bused*1000.0/sr):0.f;
                float  latMin = (sr>0)?(float)(bmin *1000.0/sr):0.f;
                float  latPrf = (sr>0)?(float)(bpref*1000.0/sr):0.f;

                if (ImGui::BeginTable("##asio_lat", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
                {
                    ImGui::TableSetupColumn(ru2 ? "Режим"    : "Mode");
                    ImGui::TableSetupColumn(ru2 ? "Семплы"   : "Samples");
                    ImGui::TableSetupColumn(ru2 ? "Задержка" : "Latency");
                    ImGui::TableHeadersRow();

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text(ru2 ? "Мин."  : "Min");
                    ImGui::TableSetColumnIndex(1);
                    if (bufChanged) ImGui::TextColored({1.f,0.85f,0.1f,1.f}, "%d (!)", bmin);
                    else            ImGui::Text("%d", bmin);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextColored({0.3f,1.0f,0.3f,1.f}, "%.1f ms", latMin);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text(ru2 ? "Текущий" : "Current");
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%d", bused);
                    ImGui::TableSetColumnIndex(2);
                    float lc = (latMs<5.f)?1.0f:(latMs<15.f)?0.7f:0.3f;
                    ImGui::TextColored({lc, 1.0f-lc*0.5f, 0.2f, 1.f}, "%.1f ms", latMs);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text(ru2 ? "Преф." : "Pref");
                    ImGui::TableSetColumnIndex(1);
                    if (bufChanged) ImGui::TextColored({1.f,0.85f,0.1f,1.f}, "%d (!)", bpref);
                    else            ImGui::Text("%d", bpref);
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f ms", latPrf);

                    ImGui::EndTable();
                }

                ImGui::Spacing();

                // ── Control Panel button ───────────────────────────────────
                ImGui::PushStyleColor(ImGuiCol_Button,        {0.15f,0.35f,0.65f,1.f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.25f,0.50f,0.85f,1.f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.10f,0.25f,0.50f,1.f});
                if (ImGui::Button(ru2 ? "  Открыть Control Panel ASIO  "
                                      : "  Open ASIO Control Panel  ", {-1, 0})) {
                    m_asioCpOpened = true;
                    asio.showControlPanel();
                    // Re-query immediately after CP closes (blocking call)
                    bool changed = asio.queryLiveSettings();
                    m_asioLive = { asio.sampleRate(), asio.bufferSize(),
                                   asio.bufferSizeMin(), asio.bufferSizePref() };
                    m_asioNeedsReload = changed;
                    if (changed)
                        LOG_INFO("ASIO: настройки изменены в Control Panel — требуется перезагрузка драйвера");
                }
                ImGui::PopStyleColor(3);

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(ru2
                        ? "Открыть GUI драйвера.\nПосле изменения настроек нажмите\n'Перезагрузить драйвер' ниже."
                        : "Open driver GUI.\nAfter changing settings, press\n'Reload Driver' below.");

                // ── Reload warning ─────────────────────────────────────────
                if (m_asioNeedsReload) {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.35f,0.25f,0.0f,1.f});
                    ImGui::BeginChild("##asio_warn", {-1, 54}, true);
                    ImGui::TextColored({1.f,0.85f,0.1f,1.f},
                        ru2 ? "! Настройки изменены в Control Panel"
                            : "! Settings changed in Control Panel");
                    ImGui::TextDisabled(
                        ru2 ? "Нажмите 'Перезагрузить', чтобы применить"
                            : "Press 'Reload' to apply changes");
                    ImGui::EndChild();
                    ImGui::PopStyleColor();

                    ImGui::PushStyleColor(ImGuiCol_Button,        {0.60f,0.35f,0.0f,1.f});
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.80f,0.50f,0.0f,1.f});
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.45f,0.25f,0.0f,1.f});
                    if (ImGui::Button(ru2 ? "  Перезагрузить драйвер  "
                                          : "  Reload Driver  ", {-1, 0})) {
                        AsioDeviceInfo cur = m_engine->asio().currentDevice();
                        int savedIn  = m_engine->asio().inputNodeId;
                        int savedOut = m_engine->asio().outputNodeId;
                        m_engine->stop();
                        m_engine->unloadAsioDriver();
                        if (m_engine->loadAsioDriver(cur, hwnd)) {
                            // Restore node IDs and re-register streams
                            m_engine->asio().inputNodeId  = savedIn;
                            m_engine->asio().outputNodeId = savedOut;
                            m_engine->registerAsioStreams();
                            m_asioSnap = { m_engine->asio().sampleRate(),
                                           m_engine->asio().bufferSize(),
                                           m_engine->asio().bufferSizeMin(),
                                           m_engine->asio().bufferSizePref() };
                            m_asioLive  = m_asioSnap;
                            m_asioNeedsReload = false;
                            m_asioCpOpened    = false;
                            m_engine->start();
                            LOG_INFO("ASIO: драйвер перезагружен, новые настройки применены");
                        }
                    }
                    ImGui::PopStyleColor(3);
                }

                ImGui::Spacing();
                if (ImGui::Button(T(Str::ASIO_UNLOAD), {-1, 0})) {
                    m_engine->unloadAsioDriver();
                    m_asioSnap = {};
                    m_asioLive = {};
                    m_asioCpOpened    = false;
                    m_asioNeedsReload = false;
                }
            }
        }
        ImGui::End();
    }
}

// ──────────────────────────────────────────────────────────────────────────────
//  Left device panel
// ──────────────────────────────────────────────────────────────────────────────
void NodeEditorUI::drawDevicePanel() {
    ImGui::TextColored({0.8f,0.8f,0.3f,1.0f}, "%s", T(Str::PANEL_DEVICES));
    ImGui::Separator();
    ImGui::TextDisabled("%s", T(Str::PANEL_DEVICES_HINT));
    ImGui::Spacing();

    const auto& devs = m_engine->wasapi().deviceList();
    float dt    = ImGui::GetIO().DeltaTime;
    float lineH = ImGui::GetTextLineHeight();
    // Width available for label text
    float availW = ImGui::GetContentRegionAvail().x - 20.0f;
    // Scrolling speed in px/s
    constexpr float kScrollPxPerSec = 40.0f;

    auto makeSection = [&](Str labelStr, bool capture, bool loopback) {
        if (!ImGui::CollapsingHeader(T(labelStr), ImGuiTreeNodeFlags_DefaultOpen)) return;
        for (auto& d : devs) {
            if (d.isCapture != capture || d.isLoopback != loopback) continue;

            std::string fullLabel = wToUtf8(d.friendlyName);
            if (d.isDefault)      fullLabel += " *";
            if (d.isVirtualCable) fullLabel += " (VB)";

            std::string key = wToUtf8(d.id) + (loopback ? "_lb" : "");
            float textW = ImGui::CalcTextSize(fullLabel.c_str()).x;
            bool  scroll = (textW > availW - 4.0f);

            float& off = m_nameScroll[key];
            if (scroll) {
                // Only scroll when hovered
                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
                    off += dt * kScrollPxPerSec;
                float maxOff = textW + 24.0f;
                if (off > maxOff) off = 0.0f;
            } else {
                off = 0.0f;
            }

            ImVec4 col = {0.85f,0.85f,0.85f,1.0f};
            if (d.isDefault)      col = {0.4f,1.0f,0.5f,1.0f};
            if (d.isVirtualCable) col = {0.8f,0.5f,1.0f,1.0f};
            ImU32 colU = ImGui::ColorConvertFloat4ToU32(col);

            // Bullet
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::Bullet();
            ImGui::PopStyleColor();
            ImGui::SameLine();

            ImVec2 textPos = ImGui::GetCursorScreenPos();
            ImVec2 clipMin = { textPos.x,          textPos.y - 1.0f };
            ImVec2 clipMax = { textPos.x + availW,  textPos.y + lineH + 2.0f };

            // Push clip rect so text scrolls inside panel
            ImGui::PushClipRect(clipMin, clipMax, true);
            if (scroll) {
                // Draw looping text: label + gap + label
                float gapW = ImGui::CalcTextSize("    ").x;
                ImGui::GetWindowDrawList()->AddText(
                    { textPos.x - off, textPos.y }, colU, fullLabel.c_str());
                ImGui::GetWindowDrawList()->AddText(
                    { textPos.x - off + textW + gapW, textPos.y }, colU, fullLabel.c_str());
            } else {
                ImGui::GetWindowDrawList()->AddText(textPos, colU, fullLabel.c_str());
            }
            ImGui::PopClipRect();

            // Invisible selectable for interaction (same row)
            ImGui::InvisibleButton(("##ib_"+key).c_str(),
                                   { availW, lineH });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", fullLabel.c_str());
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                int id = m_engine->addWasapiNode(d);
                auto* n = m_graph->findNode(id);
                if (n) {
                    n->posX = 150 + (float)(rand() % 200);
                    n->posY = 100 + (float)(rand() % 300);
                }
                if (m_engineStarted) { m_engine->stop(); m_engine->start(); }
            }
        }
        ImGui::Spacing();
    };

    makeSection(Str::PANEL_MICROPHONES, true,  false);
    makeSection(Str::PANEL_LOOPBACK,    false, true);
    makeSection(Str::PANEL_SPEAKERS,    false, false);
}

// ──────────────────────────────────────────────────────────────────────────────
//  Center node editor
// ──────────────────────────────────────────────────────────────────────────────
void NodeEditorUI::drawNodeEditor() {
    ImGui::TextColored({0.6f,0.8f,1.0f,1.0f}, "%s", T(Str::GRAPH_TITLE));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T(Str::GRAPH_HINT));

    // ── Pan toolbar ────────────────────────────────────────────────────────────
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 90.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4,2});
    bool ru = (g_lang == Lang::RU);
    if (ImGui::SmallButton(ru ? "Центр" : "Center")) {
        float minX = 1e9f, minY = 1e9f;
        {
            std::lock_guard<std::mutex> lk(m_graph->mutex);
            for (auto& n : m_graph->nodes()) {
                minX = std::min(minX, n.posX);
                minY = std::min(minY, n.posY);
            }
        }
        if (minX < 1e8f)
            ImNodes::EditorContextResetPanning(ImVec2{80.0f - minX, 80.0f - minY});
        else
            ImNodes::EditorContextResetPanning(ImVec2{0.0f, 0.0f});
    }
    ImGui::PopStyleVar();
    ImGui::Separator();

    ImNodes::BeginNodeEditor();

    // ── Draw nodes ──────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(m_graph->mutex);
        for (auto& node : m_graph->nodes()) {

            // FIX: SetNodeGridSpacePos must be called ONLY ONCE per node.
            // Calling it every frame locks the node in place and prevents dragging.
            if (m_positionedNodes.find(node.id) == m_positionedNodes.end()) {
                ImNodes::SetNodeGridSpacePos(node.id, {node.posX, node.posY});
                m_positionedNodes.insert(node.id);
            }

            // Read back current position (user may have dragged it)
            ImVec2 pos = ImNodes::GetNodeGridSpacePos(node.id);
            node.posX = pos.x;
            node.posY = pos.y;

            // Title color
            ImVec4 c = nodeColor(node.type);
            ImNodes::PushColorStyle(ImNodesCol_TitleBar,
                IM_COL32((int)(c.x*255),(int)(c.y*255),(int)(c.z*255),200));
            ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered,
                IM_COL32((int)(c.x*255+20),(int)(c.y*255+20),(int)(c.z*255+20),220));

            ImNodes::BeginNode(node.id);

            // Title
            ImNodes::BeginNodeTitleBar();
            ImGui::TextUnformatted(
                (truncate(node.name,22) + deviceTypeIcon(node.type)).c_str());
            ImNodes::EndNodeTitleBar();

            // ── Format info line ──────────────────────────────────────────
            bool isAsioNode = (node.type == DeviceType::AsioInput ||
                               node.type == DeviceType::AsioOutput);
            if (isAsioNode && m_engine->asio().isLoaded()) {
                // Pull live info from ASIO driver
                double sr   = m_engine->asio().sampleRate();
                int    buf  = m_engine->asio().bufferSize();
                int    ch   = (node.type == DeviceType::AsioInput)
                                ? m_engine->asio().numInputChannels()
                                : m_engine->asio().numOutputChannels();
                float  ms   = (sr > 0) ? (float)(buf * 1000.0 / sr) : 0.f;
                char   fmtBuf[48];
                snprintf(fmtBuf, sizeof(fmtBuf),
                         "%.0f Hz | 32bit | %dch | %.1fms", sr, ch, ms);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.7f,0.7f,0.7f,1.f});
                ImGui::TextUnformatted(fmtBuf);
                ImGui::PopStyleColor();
            } else {
                // WASAPI: show rt info if stream is open, else show node settings
                auto rti = m_engine->wasapi().getRtInfo(node.id);
                char fmtBuf[48] = "";
                if (rti.sampleRate > 0) {
                    snprintf(fmtBuf, sizeof(fmtBuf),
                             "%d Hz | %dbit | %dch",
                             rti.sampleRate, rti.bitDepth, rti.channels);
                } else if (node.deviceSampleRate > 0 || node.deviceChannels > 0) {
                    int sr = node.deviceSampleRate > 0 ? node.deviceSampleRate : 0;
                    int ch = node.deviceChannels   > 0 ? node.deviceChannels   : 0;
                    if (sr > 0 && ch > 0)
                        snprintf(fmtBuf, sizeof(fmtBuf), "%d Hz | %dch", sr, ch);
                    else if (sr > 0)
                        snprintf(fmtBuf, sizeof(fmtBuf), "%d Hz", sr);
                    else if (ch > 0)
                        snprintf(fmtBuf, sizeof(fmtBuf), "%dch", ch);
                }
                if (fmtBuf[0]) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.6f,0.9f,0.6f,1.f});
                    ImGui::TextUnformatted(fmtBuf);
                    ImGui::PopStyleColor();
                }
            }

            // Body: level meter
            drawMiniMeter(node.peakL);
            if (node.type != DeviceType::WasapiCapture &&
                node.type != DeviceType::AsioInput) {
                ImGui::SameLine();
                drawMiniMeter(node.peakR);
            }

            // Gain knob (small)
            ImGui::SetNextItemWidth(80);
            float g = node.gainDb;
            if (ImGui::SliderFloat(("##g"+std::to_string(node.id)).c_str(),
                                   &g, -40.0f, 12.0f, "%.1f dB"))
            {
                node.gainDb = g;
                m_graph->clearDirty();
            }

            // Enable toggle
            ImGui::SameLine();
            bool en = node.enabled;
            if (ImGui::Checkbox(("##en"+std::to_string(node.id)).c_str(), &en))
                node.enabled = en;

            // Input pins
            for (auto& pin : node.inputs) {
                ImNodes::BeginInputAttribute(encodePinId(node.id, pin.pinIndex, true));
                ImGui::TextDisabled("in");
                ImNodes::EndInputAttribute();
            }
            // Output pins
            for (auto& pin : node.outputs) {
                ImNodes::BeginOutputAttribute(encodePinId(node.id, pin.pinIndex, false));
                ImGui::TextDisabled("out");
                ImNodes::EndOutputAttribute();
            }

            ImNodes::EndNode();
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();

            // Position already read back at line 380 — no duplicate needed
        }

        // Draw links (edges) — highlight hovered/selected
        for (auto& edge : m_graph->edges()) {
            int fromPin = encodePinId(edge.fromNodeId, edge.fromPinIndex, false);
            int toPin   = encodePinId(edge.toNodeId,   edge.toPinIndex,   true);
            ImNodes::Link(edge.id, fromPin, toPin);
        }
    }

    ImNodes::EndNodeEditor();

    // ── Handle new link creation ──────────────────────────────────────────
    int fromPin, toPin;
    if (ImNodes::IsLinkCreated(&fromPin, &toPin)) {
        int fromNode, fromPinIdx; bool fromIsIn;
        int toNode,   toPinIdx;  bool toIsIn;
        decodePinId(fromPin, fromNode, fromPinIdx, fromIsIn);
        decodePinId(toPin,   toNode,   toPinIdx,   toIsIn);
        if (!fromIsIn && toIsIn) {
            m_graph->addEdge(fromNode, fromPinIdx, toNode, toPinIdx);
            if (m_engineStarted) { m_engine->stop(); m_engine->start(); }
        }
    }

    // ── Handle link deletion (drag-detach by imnodes) ─────────────────────
    int destroyedLink;
    if (ImNodes::IsLinkDestroyed(&destroyedLink))
        m_pendingDeleteEdge = destroyedLink;

    // ── Delete selected links / nodes with Delete or Backspace ────────────
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) ||
        ImGui::IsKeyPressed(ImGuiKey_Backspace))
    {
        // Delete selected links
        int numSelLinks = ImNodes::NumSelectedLinks();
        if (numSelLinks > 0) {
            std::vector<int> selLinks(numSelLinks);
            ImNodes::GetSelectedLinks(selLinks.data());
            for (int lid : selLinks) {
                m_graph->removeEdge(lid);
            }
            ImNodes::ClearLinkSelection();
            if (m_engineStarted) { m_engine->stop(); m_engine->start(); }
        }
        // Delete selected nodes
        int numSelNodes = ImNodes::NumSelectedNodes();
        if (numSelNodes > 0) {
            std::vector<int> selNodes(numSelNodes);
            ImNodes::GetSelectedNodes(selNodes.data());
            for (int nid : selNodes) {
                m_graph->removeNode(nid);
                m_positionedNodes.erase(nid);
                m_engine->wasapi().closeStream(nid);
            }
            ImNodes::ClearNodeSelection();
            m_selectedNodeId = -1;
            if (m_engineStarted) { m_engine->stop(); m_engine->start(); }
        }
    }

    // ── Handle node selection ─────────────────────────────────────────────
    if (ImNodes::NumSelectedNodes() == 1) {
        int sel;
        ImNodes::GetSelectedNodes(&sel);
        m_selectedNodeId = sel;
    } else if (ImNodes::NumSelectedNodes() == 0 &&
               ImNodes::NumSelectedLinks() == 0) {
        m_selectedNodeId = -1;
    }

    // ── Right-click on LINK → context menu ───────────────────────────────
    {
        int hoveredLink = -1;
        if (ImNodes::IsLinkHovered(&hoveredLink) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_rightClickEdge = hoveredLink;
            ImGui::OpenPopup("##linkCtx");
        }
    }
    if (ImGui::BeginPopup("##linkCtx")) {
        bool ru = (g_lang == Lang::RU);
        if (ImGui::MenuItem(ru ? "Разъединить" : "Disconnect")) {
            if (m_rightClickEdge >= 0) {
                m_pendingDeleteEdge = m_rightClickEdge;
                m_rightClickEdge = -1;
            }
        }
        ImGui::EndPopup();
    }

    // ── Right-click on NODE → context menu ───────────────────────────────
    {
        int hoveredNode = -1;
        if (ImNodes::IsNodeHovered(&hoveredNode) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_rightClickNode = hoveredNode;
            ImGui::OpenPopup("##nodeCtx");
        }
    }
    if (ImGui::BeginPopup("##nodeCtx")) {
        bool ru = (g_lang == Lang::RU);
        if (ImGui::MenuItem(ru ? "Удалить все соединения" : "Disconnect all")) {
            if (m_rightClickNode >= 0) {
                std::vector<int> toRemove;
                {
                    std::lock_guard<std::mutex> lk(m_graph->mutex);
                    for (auto& e : m_graph->edges())
                        if (e.fromNodeId == m_rightClickNode ||
                            e.toNodeId   == m_rightClickNode)
                            toRemove.push_back(e.id);
                }
                for (int eid : toRemove)
                    m_graph->removeEdge(eid);
                if (!toRemove.empty() && m_engineStarted)
                { m_engine->stop(); m_engine->start(); }
                m_rightClickNode = -1;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T(Str::PROPS_DELETE))) {
            if (m_rightClickNode >= 0) {
                m_pendingDeleteNode = m_rightClickNode;
                m_rightClickNode = -1;
            }
        }
        ImGui::EndPopup();
    }
}

// ──────────────────────────────────────────────────────────────────────────────
//  Right properties panel
// ──────────────────────────────────────────────────────────────────────────────
void NodeEditorUI::drawPropertiesPanel() {
    bool ru = (g_lang == Lang::RU);
    ImGui::TextColored({0.8f,0.8f,0.3f,1.0f}, "%s", T(Str::PROPS_TITLE));
    ImGui::Separator();

    if (m_selectedNodeId < 0) {
        ImGui::TextDisabled("%s", T(Str::PROPS_HINT));
        return;
    }

    std::lock_guard<std::mutex> lk(m_graph->mutex);
    auto* n = m_graph->findNode(m_selectedNodeId);
    if (!n) return;

    ImGui::Text("%s %d",  T(Str::PROPS_ID),   n->id);
    ImGui::Text("%s %s",  T(Str::PROPS_TYPE),  deviceTypeIcon(n->type));
    ImGui::Separator();

    static char nameBuf[64];
    if (ImGui::IsWindowAppearing())
        strncpy_s(nameBuf, n->name.c_str(), sizeof(nameBuf)-1);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
        n->name = nameBuf;

    ImGui::Spacing();
    ImGui::Text("%s", T(Str::PROPS_GAIN));
    ImGui::SetNextItemWidth(-30.0f);
    ImGui::SliderFloat("##gain", &n->gainDb, -40.0f, 12.0f, "%.1f dB");
    // Right-click → manual text entry popup
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##gainInput");
    if (ImGui::BeginPopup("##gainInput")) {
        ImGui::Text("dB:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputFloat("##gainVal", &n->gainDb, 0, 0, "%.2f",
                              ImGuiInputTextFlags_EnterReturnsTrue))
        {
            n->gainDb = std::clamp(n->gainDb, -40.0f, 12.0f);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("0")) n->gainDb = 0.0f;

    ImGui::Spacing();
    ImGui::Checkbox(T(Str::PROPS_ENABLED), &n->enabled);

    ImGui::Separator();
    ImGui::Text("%s %.3f", T(Str::PROPS_PEAK_L), n->peakL);
    ImGui::Text("%s %.3f", T(Str::PROPS_PEAK_R), n->peakR);
    float dbL = n->peakL > 0.0001f ? 20.0f * std::log10(n->peakL) : -60.0f;
    float dbR = n->peakR > 0.0001f ? 20.0f * std::log10(n->peakR) : -60.0f;
    ImGui::Text("dBFS L: %.1f  R: %.1f", dbL, dbR);

    // Full-width level bars
    ImGui::Spacing();
    ImVec2 barSize = {ImGui::GetContentRegionAvail().x, 12};
    ImVec2 pos = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    auto drawBar = [&](float peak, ImVec2 p) {
        float w = barSize.x;
        dl->AddRectFilled(p, {p.x+w, p.y+barSize.y}, IM_COL32(40,40,40,200));
        float fill = std::clamp(peak, 0.0f, 1.0f) * w;
        ImU32 col = peak < 0.7f ? IM_COL32(50,200,80,255) :
                    peak < 0.9f ? IM_COL32(220,180,30,255) :
                                  IM_COL32(220,50,50,255);
        if (fill > 0) dl->AddRectFilled(p, {p.x+fill, p.y+barSize.y}, col);
        dl->AddRect(p, {p.x+w, p.y+barSize.y}, IM_COL32(100,100,100,200));
    };
    drawBar(n->peakL, pos);
    ImGui::Dummy({barSize.x, barSize.y+2});
    pos = ImGui::GetCursorScreenPos();
    drawBar(n->peakR, pos);
    ImGui::Dummy({barSize.x, barSize.y+2});

    // ── Noise Filter section (capture nodes only) ────────────────────────────
    if (n->type == DeviceType::WasapiCapture || n->type == DeviceType::AsioInput)
    {
        ImGui::Separator();
        bool ru = (g_lang == Lang::RU);
        ImGui::TextColored({0.6f,0.9f,0.6f,1.0f}, "%s",
                           ru ? "ШУМОПОДАВЛЕНИЕ" : "NOISE SUPPRESSION");

        const char* modeNamesRU[] = { "Выкл", "Лёгкий", "Средний", "Агрессивный", "RNNoise" };
        const char* modeNamesEN[] = { "Off",  "Light",   "Medium",  "Aggressive",  "RNNoise" };
        const char** names = ru ? modeNamesRU : modeNamesEN;

        ImVec4 btnCols[] = {
            {0.25f,0.25f,0.28f,1.0f},   // Off
            {0.18f,0.45f,0.22f,1.0f},   // Light
            {0.20f,0.38f,0.55f,1.0f},   // Medium
            {0.50f,0.22f,0.18f,1.0f},   // Aggressive
            {0.40f,0.20f,0.55f,1.0f},   // RNNoise — purple
        };
        float bw = (ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f;
        for (int m = 0; m < 5; ++m) {
            bool selected = (n->noiseFilterMode == m);
            ImVec4 col   = btnCols[m];
            ImVec4 colHov= {col.x+0.1f, col.y+0.1f, col.z+0.1f, 1.0f};
            if (selected) { col.x += 0.15f; col.y += 0.15f; col.z += 0.15f; }
            ImGui::PushStyleColor(ImGuiCol_Button,        col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colHov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  colHov);
            if (ImGui::Button(names[m], {m == 4 ? -1.0f : bw, 0}))
                n->noiseFilterMode = m;
            ImGui::PopStyleColor(3);
            if (m % 2 == 0 && m < 4) ImGui::SameLine(0, 3);
            else if (m < 3)          ImGui::Spacing();
        }

        ImGui::Spacing();
        const char* descRU[] = {
            "Шумодав отключен.",
            "ФВЧ 85 Гц + гейт.\nУдаляет гул и тихий шум.",
            "Спектральное вычитание.\nУдаляет вентилятор, кондиционер.",
            "Глубокое спектральное вычитание.\nМаксимальное подавление.",
            "RNNoise — нейросеть Mozilla/Xiph.\nЛучшее качество, удаляет\nлюбой нестационарный шум.\nРаботает как Krisp/Discord.",
        };
        const char* descEN[] = {
            "Noise suppression off.",
            "High-pass 85Hz + gate.\nRemoves rumble and quiet noise.",
            "Spectral subtraction.\nRemoves fan, AC hum.",
            "Deep spectral subtraction.\nMaximum suppression.",
            "RNNoise — Mozilla/Xiph neural net.\nBest quality, removes any noise.\nWorks like Krisp/Discord.",
        };
        ImGui::PushStyleColor(ImGuiCol_Text, {0.6f,0.6f,0.6f,1.0f});
        ImGui::TextWrapped("%s", ru ? descRU[n->noiseFilterMode] : descEN[n->noiseFilterMode]);
        ImGui::PopStyleColor();

        // Noise gate threshold (Light / Medium / Aggressive)
        if (n->noiseFilterMode > 0) {
            ImGui::Spacing();
            ImGui::Text("%s", ru ? "Порог гейта:" : "Gate threshold:");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##gateThresh",
                               &n->noiseGateThreshDb, -60.0f, -10.0f, "%.0f dB");
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("##gateInput");
            if (ImGui::BeginPopup("##gateInput")) {
                ImGui::Text("dB:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputFloat("##gateVal", &n->noiseGateThreshDb, 0, 0, "%.1f",
                                      ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    n->noiseGateThreshDb = std::clamp(n->noiseGateThreshDb, -60.0f, -10.0f);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(ru
                    ? "ПКМ — ввести вручную\nРекомендуется: -40...-30 дБ"
                    : "Right-click to type value\nRecommended: -40...-30 dB");

            // Live gate meter from filter instance
            if (n->noiseFilter) {
                float gL = n->noiseFilter->gateGainL.load();
                float gR = n->noiseFilter->gateGainR.load();
                ImGui::Spacing();
                ImGui::TextDisabled("%s", ru ? "Гейт L/R:" : "Gate L/R:");
                ImVec2 p2 = ImGui::GetCursorScreenPos();
                float w2  = ImGui::GetContentRegionAvail().x;
                float h2  = 7.0f;
                // L bar
                dl->AddRectFilled(p2, {p2.x+w2, p2.y+h2}, IM_COL32(30,30,30,200));
                dl->AddRectFilled(p2, {p2.x+gL*w2, p2.y+h2},
                                  gL > 0.5f ? IM_COL32(60,200,80,200) : IM_COL32(200,80,50,200));
                dl->AddRect(p2, {p2.x+w2, p2.y+h2}, IM_COL32(80,80,80,180));
                ImGui::Dummy({w2, h2+1});
                ImVec2 p3 = ImGui::GetCursorScreenPos();
                dl->AddRectFilled(p3, {p3.x+w2, p3.y+h2}, IM_COL32(30,30,30,200));
                dl->AddRectFilled(p3, {p3.x+gR*w2, p3.y+h2},
                                  gR > 0.5f ? IM_COL32(60,200,80,200) : IM_COL32(200,80,50,200));
                dl->AddRect(p3, {p3.x+w2, p3.y+h2}, IM_COL32(80,80,80,180));
                ImGui::Dummy({w2, h2+1});
            }
        }
    }

    // ── Format / Channel / Sample Rate section ────────────────────────────
    ImGui::Separator();
    bool isCapture = (n->type == DeviceType::WasapiCapture ||
                      n->type == DeviceType::WasapiLoopback ||
                      n->type == DeviceType::AsioInput);
    bool isRender  = (n->type == DeviceType::WasapiRender ||
                      n->type == DeviceType::VirtualSink  ||
                      n->type == DeviceType::AsioOutput);
    bool isWasapi  = (n->type != DeviceType::AsioInput &&
                      n->type != DeviceType::AsioOutput &&
                      n->type != DeviceType::Mixer);
    bool isAsio    = (n->type == DeviceType::AsioInput ||
                      n->type == DeviceType::AsioOutput);

    if (isCapture || isRender) {
        ImGui::TextColored({0.5f,0.8f,1.0f,1.0f},
            ru ? "Формат устройства" : "Device Format");

        // Show actual runtime format if engine is running
        auto rti = m_engine->wasapi().getRtInfo(n->id);
        if (rti.sampleRate > 0) {
            ImGui::TextDisabled(ru ? "Активно: %d Hz / %d bit / %dch"
                                   : "Active: %d Hz / %d bit / %dch",
                                rti.sampleRate, rti.bitDepth, rti.channels);
        }

        // Channels
        ImGui::Text(ru ? "Каналы:" : "Channels:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        const char* chItems[] = { ru ? "Авто" : "Auto",
                                   "Mono (1ch)",
                                   "Stereo (2ch)" };
        int chIdx = std::clamp(n->deviceChannels, 0, 2);
        if (ImGui::Combo("##dch", &chIdx, chItems, 3))
            n->deviceChannels = chIdx;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(ru ? "Изменение применится после перезапуска"
                                 : "Change applies after restart");

        // Sample rate (only for WASAPI capture — render uses system rate)
        if (isCapture && isWasapi) {
            ImGui::Text(ru ? "Частота дискр.:" : "Sample Rate:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            const char* srItems[] = { ru ? "Авто" : "Auto",
                                       "44100 Hz", "48000 Hz",
                                       "88200 Hz", "96000 Hz" };
            int srVals[] = { 0, 44100, 48000, 88200, 96000 };
            int srIdx = 0;
            for (int i = 1; i < 5; ++i)
                if (n->deviceSampleRate == srVals[i]) { srIdx = i; break; }
            if (ImGui::Combo("##dsr", &srIdx, srItems, 5))
                n->deviceSampleRate = srVals[srIdx];
        }

        // ASIO: min buffer checkbox
        if (isAsio) {
            ImGui::Spacing();
            ImGui::Checkbox(ru ? "Минимальный буфер (мин. задержка)"
                               : "Minimum buffer (lowest latency)",
                            &n->asioMinBuffer);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(ru
                    ? "Использует наименьший буфер драйвера.\n"
                      "Если слышны щелчки — отключите."
                    : "Uses the smallest driver buffer.\n"
                      "If you hear clicks, disable this.");

            // Show ASIO buffer info
            if (m_engine->asio().isLoaded()) {
                int bmin  = m_engine->asio().bufferSizeMin();
                int bpref = m_engine->asio().bufferSizePref();
                double sr = m_engine->asio().sampleRate();
                float latMin  = sr > 0 ? (bmin  * 1000.0f / (float)sr) : 0;
                float latPref = sr > 0 ? (bpref * 1000.0f / (float)sr) : 0;
                ImGui::TextDisabled(ru ? "Мин: %d сэмп / %.1f мс\nПреф: %d сэмп / %.1f мс"
                                       : "Min: %d smp / %.1f ms\nPref: %d smp / %.1f ms",
                                    bmin, latMin, bpref, latPref);
            }
        }

        if (isWasapi) {
            ImGui::Spacing();
            ImGui::TextDisabled(ru
                ? "Частота дискретизации WASAPI задаётся\n"
                  "в Панели управления Windows:\n"
                  "Звук > Свойства > Дополнительно"
                : "WASAPI sample rate is set in\n"
                  "Windows Control Panel:\n"
                  "Sound > Properties > Advanced");
        }
    }

    // ── ASIO: info + driver install instructions ───────────────────────────
    if (isAsio && !m_engine->asio().isLoaded()) {
        ImGui::Spacing();
        ImGui::TextColored({1.0f,0.6f,0.2f,1.0f},
            ru ? "! Драйвер не загружен" : "! Driver not loaded");
        ImGui::TextWrapped(ru
            ? "ASIO использует установленные системные драйверы.\n"
              "Для Behringer:\n"
              "1. Диспетчер устройств\n"
              "2. Обновить драйвер\n"
              "3. Указать папку BEHRINGER_2902_X64\n"
              "После установки нажмите 'Загрузить ASIO'."
            : "ASIO uses system-installed drivers.\n"
              "For Behringer:\n"
              "1. Device Manager\n"
              "2. Update driver\n"
              "3. Point to BEHRINGER_2902_X64 folder\n"
              "Then press 'Load ASIO'.");
    }

    if (isAsio && m_engine->asio().isLoaded()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.15f,0.35f,0.65f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.25f,0.50f,0.85f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.10f,0.25f,0.50f,1.f});
        if (ImGui::Button(ru ? "ASIO Control Panel" : "ASIO Control Panel", {-1, 0}))
            m_engine->asio().showControlPanel();
        ImGui::PopStyleColor(3);
        ImGui::TextDisabled(ru
            ? "Настройка частоты, буфера и бит\nвыполняется в панели драйвера"
            : "Configure SR, buffer and bit depth\nin the driver control panel");
    }

    ImGui::Separator();
    if (ImGui::Button(T(Str::PROPS_DELETE), {-1, 0}))
        m_pendingDeleteNode = m_selectedNodeId;
}

// ──────────────────────────────────────────────────────────────────────────────
//  Status bar
// ──────────────────────────────────────────────────────────────────────────────
void NodeEditorUI::drawStatusBar() {
    ImGui::Separator();
    auto& devs = m_engine->wasapi().deviceList();
    int capCnt = 0, renCnt = 0, virtCnt = 0;
    for (auto& d : devs) {
        if (d.isVirtualCable) virtCnt++;
        else if (d.isCapture) capCnt++;
        else if (!d.isLoopback) renCnt++;
    }
    ImGui::Text("  %d %s | %d %s | %d %s  |  %s %d  |  %s %d  |  %s",
                capCnt,  T(Str::STATUS_CAPTURE),
                renCnt,  T(Str::STATUS_RENDER),
                virtCnt, T(Str::STATUS_VIRTUAL),
                T(Str::STATUS_NODES),       (int)m_graph->nodes().size(),
                T(Str::STATUS_CONNECTIONS), (int)m_graph->edges().size(),
                m_engineStarted ? T(Str::STATUS_ROUTING) : T(Str::STATUS_STOPPED));
}

// ──────────────────────────────────────────────────────────────────────────────
//  Settings window
// ──────────────────────────────────────────────────────────────────────────────
void NodeEditorUI::drawSettingsWindow() {
    if (!m_showSettingsWindow) return;

    ImGui::SetNextWindowSize({400, 320}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({200, 150}, ImGuiCond_FirstUseEver);

    const char* title = (g_lang == Lang::RU) ? "  Настройки" : "  Settings";
    if (!ImGui::Begin(title, &m_showSettingsWindow)) {
        ImGui::End(); return;
    }

    // ── Язык ──────────────────────────────────────────────────────────────
    ImGui::SeparatorText((g_lang == Lang::RU) ? "Язык / Language" : "Language");
    int lang = (int)g_lang;
    ImGui::RadioButton(T(Str::SETTINGS_RUSSIAN), &lang, 0); ImGui::SameLine();
    ImGui::RadioButton(T(Str::SETTINGS_ENGLISH), &lang, 1);
    g_lang = (Lang)lang;

    // ── Масштаб UI ────────────────────────────────────────────────────────
    ImGui::SeparatorText((g_lang == Lang::RU) ? "Масштаб интерфейса" : "UI Scale");
    float scales[] = { 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };
    const char* scaleLabels[] = { "75%", "100%", "125%", "150%", "200%" };
    for (int i = 0; i < 5; ++i) {
        bool sel = (fabsf(g_settings.uiScale - scales[i]) < 0.01f);
        if (ImGui::RadioButton(scaleLabels[i], sel)) {
            g_settings.uiScale = scales[i];
            ImGui::GetIO().FontGlobalScale = scales[i];
        }
        if (i < 4) ImGui::SameLine();
    }
    ImGui::TextDisabled((g_lang == Lang::RU)
        ? "(перезапустите для применения шрифта)"
        : "(restart to apply font size)");

    // ── Автозапуск ────────────────────────────────────────────────────────
    ImGui::SeparatorText((g_lang == Lang::RU) ? "Автозапуск" : "Autostart");

    bool autostart = AppSettings::isAutostartEnabled();
    if (ImGui::Checkbox((g_lang == Lang::RU)
                        ? "Запускать с Windows"
                        : "Start with Windows", &autostart)) {
        g_settings.autostart = autostart;
        g_settings.applyAutostart();
    }

    bool startMin = g_settings.startMinimized;
    if (ImGui::Checkbox((g_lang == Lang::RU)
                        ? "Запускать свёрнутым в трей"
                        : "Start minimized to tray", &startMin))
        g_settings.startMinimized = startMin;

    // ── Патч ─────────────────────────────────────────────────────────────
    ImGui::SeparatorText((g_lang == Lang::RU)
                         ? "Патч при запуске" : "Startup Patch");
    bool autoLoad = g_settings.autoLoadLastPatch;
    if (ImGui::Checkbox((g_lang == Lang::RU)
                        ? "Загружать последний патч при запуске"
                        : "Auto-load last patch on startup", &autoLoad))
        g_settings.autoLoadLastPatch = autoLoad;

    if (!g_settings.lastPatch.empty()) {
        ImGui::TextDisabled("%s: %s",
            (g_lang == Lang::RU) ? "Последний" : "Last",
            g_settings.lastPatch.c_str());
    }

    ImGui::Spacing();
    if (ImGui::Button((g_lang == Lang::RU) ? "Сохранить" : "Save", {120, 0})) {
        g_settings.language = lang;
        extern std::string g_settingsPath;
        g_settings.save(g_settingsPath);
        m_showSettingsWindow = false;
    }
    ImGui::SameLine();
    if (ImGui::Button((g_lang == Lang::RU) ? "Закрыть" : "Close", {120, 0}))
        m_showSettingsWindow = false;

    ImGui::End();
}
void NodeEditorUI::drawMiniMeter(float peak, float width) {
    ImVec2 pos  = ImGui::GetCursorScreenPos();
    ImVec2 size = {width, 8.0f};
    auto*  dl   = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x+size.x, pos.y+size.y}, IM_COL32(30,30,30,220));
    float fill = std::clamp(peak, 0.0f, 1.0f) * size.x;
    if (fill > 0) {
        ImU32 col = peak < 0.7f ? IM_COL32(60,200,80,230) :
                    peak < 0.9f ? IM_COL32(220,180,30,230) :
                                  IM_COL32(220,50,50,255);
        dl->AddRectFilled(pos, {pos.x+fill, pos.y+size.y}, col);
    }
    dl->AddRect(pos, {pos.x+size.x, pos.y+size.y}, IM_COL32(100,100,100,180));
    ImGui::Dummy({size.x, size.y + 2});
}

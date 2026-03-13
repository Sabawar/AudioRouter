#include "Logger.h"
#include "Lang.h"
#include "imgui.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <debugapi.h>
#include <ctime>
#include <cstring>
#include <algorithm>

// ──────────────────────────────────────────────────────────────────────────────
//  open / close
// ──────────────────────────────────────────────────────────────────────────────
void Logger::open(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_file.open(path, std::ios::out | std::ios::trunc);
    if (m_file.is_open()) {
        m_file << "========================================\n";
        m_file << "  AudioRouter Log\n";
        m_file << "========================================\n";
        m_file.flush();
    }
}

void Logger::close() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_file.is_open()) m_file.close();
}

// ──────────────────────────────────────────────────────────────────────────────
//  log()
// ──────────────────────────────────────────────────────────────────────────────
std::string Logger::makeTimestamp() {
    auto now   = std::chrono::system_clock::now();
    auto tt    = std::chrono::system_clock::to_time_t(now);
    auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now.time_since_epoch()) % 1000;
    std::tm tm_buf {};
#ifdef _WIN32
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, (int)ms.count());
    return buf;
}

const char* Logger::levelStr(LogLevel l) {
    switch (l) {
    case LogLevel::Debug: return "DBG";
    case LogLevel::Info:  return "INF";
    case LogLevel::Warn:  return "WRN";
    case LogLevel::Error: return "ERR";
    case LogLevel::Fatal: return "FAT";
    }
    return "???";
}

void Logger::log(LogLevel level, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::string ts  = makeTimestamp();
    std::string msg = buf;

    // Format: [HH:MM:SS.mmm][LVL] message
    char line[2112];
    snprintf(line, sizeof(line), "[%s][%s] %s", ts.c_str(), levelStr(level), buf);

    {
        std::lock_guard<std::mutex> lk(m_mutex);

        // File
        if (m_file.is_open()) {
            m_file << line << "\n";
            m_file.flush();
        }

        // Ring buffer
        if (m_entries.size() >= kMaxEntries)
            m_entries.erase(m_entries.begin());

        LogEntry entry;
        entry.level     = level;
        entry.timestamp = ts;
        entry.message   = msg;
        m_entries.push_back(std::move(entry));
    }

    // Also output to debugger
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}

// ──────────────────────────────────────────────────────────────────────────────
//  ImGui log window
// ──────────────────────────────────────────────────────────────────────────────
void Logger::drawWindow(bool* p_open) {
    if (!*p_open) return;

    ImGui::SetNextWindowSize({760, 300}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({100, 530}, ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.08f, 0.08f, 0.09f, 0.97f});
    if (!ImGui::Begin(T(Str::LOG_TITLE), p_open,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }
    ImGui::PopStyleColor();

    // ── Toolbar ────────────────────────────────────────────────────────────
    if (ImGui::SmallButton(T(Str::LOG_CLEAR)))  clear();
    ImGui::SameLine();
    ImGui::Checkbox(T(Str::LOG_AUTOSCROLL), &autoScroll);
    ImGui::SameLine();

    // Level filter
    static bool showDebug = true, showInfo = true,
                showWarn  = true, showError = true;
    ImGui::PushStyleColor(ImGuiCol_Button, {0.3f,0.3f,0.35f,1.0f});
    if (ImGui::SmallButton(showDebug ? "[DBG]" : " DBG ")) showDebug = !showDebug;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, {0.15f,0.35f,0.15f,1.0f});
    if (ImGui::SmallButton(showInfo  ? "[INF]" : " INF ")) showInfo  = !showInfo;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, {0.45f,0.35f,0.05f,1.0f});
    if (ImGui::SmallButton(showWarn  ? "[WRN]" : " WRN ")) showWarn  = !showWarn;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, {0.50f,0.10f,0.10f,1.0f});
    if (ImGui::SmallButton(showError ? "[ERR]" : " ERR ")) showError = !showError;
    ImGui::PopStyleColor(4);

    ImGui::Separator();

    // ── Scrollable area ────────────────────────────────────────────────────
    ImGui::BeginChild("##logscroll", {0,0}, false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4, 1});

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (const auto& e : m_entries) {
            if (!showDebug && e.level == LogLevel::Debug) continue;
            if (!showInfo  && e.level == LogLevel::Info)  continue;
            if (!showWarn  && e.level == LogLevel::Warn)  continue;
            if (!showError && (e.level == LogLevel::Error ||
                               e.level == LogLevel::Fatal)) continue;

            // Color by level
            ImVec4 col;
            switch (e.level) {
            case LogLevel::Debug: col = {0.50f,0.50f,0.55f,1.0f}; break;
            case LogLevel::Info:  col = {0.85f,0.85f,0.85f,1.0f}; break;
            case LogLevel::Warn:  col = {0.95f,0.78f,0.20f,1.0f}; break;
            case LogLevel::Error: col = {0.95f,0.30f,0.25f,1.0f}; break;
            case LogLevel::Fatal: col = {1.00f,0.10f,0.10f,1.0f}; break;
            }

            // Timestamp in grey
            ImGui::TextColored({0.45f,0.45f,0.50f,1.0f},
                               "[%s]", e.timestamp.c_str());
            ImGui::SameLine();
            // Level badge
            ImGui::TextColored(col, "[%s]", levelStr(e.level));
            ImGui::SameLine();
            // Message
            ImGui::TextColored(col, "%s", e.message.c_str());
        }
    }

    ImGui::PopStyleVar();

    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}

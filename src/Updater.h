#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  Updater.h — автообновление AudioRouter через GitHub Releases
//
//  Idle → Checking → UpdateAvailable → Downloading → ReadyToInstall → (restart)
//                                    └→ UpToDate
//                                    └→ Error
// ─────────────────────────────────────────────────────────────────────────────
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <cstdio>
#include <cstring>
#include "Logger.h"
#include "Lang.h"
#include "imgui.h"

// ── Конфигурация ──────────────────────────────────────────────────────────────
#define UPDATER_OWNER           "Sabawar"
#define UPDATER_REPO            "AudioRouter"
#define UPDATER_CURRENT_VER     "1.0.0"
#define UPDATER_CHECK_ON_START  true

struct VersionInfo {
    std::string tag, version, releaseNotes, downloadUrl, htmlUrl;
};

// ─────────────────────────────────────────────────────────────────────────────
class Updater {
public:
    enum class State {
        Idle, Checking, UpdateAvailable, Downloading, ReadyToInstall, UpToDate, Error
    };

    static Updater& get() { static Updater inst; return inst; }

    void startCheck() {
        if (m_state==State::Checking || m_state==State::Downloading) return;
        setState(State::Checking);
        m_error = "";
        m_info  = {};
        joinWorker();
        m_worker = std::thread([this]{ workerCheck(); });
    }

    void startDownload() {
        if (m_state != State::UpdateAvailable) return;
        setState(State::Downloading);
        m_downloadProgress = 0.f;
        m_downloadedBytes  = 0;
        m_totalBytes       = 0;
        m_downloadedPath.clear();
        joinWorker();
        m_worker = std::thread([this]{ workerDownload(); });
    }

    void applyUpdate(HWND hwnd) {
        if (m_state != State::ReadyToInstall || m_downloadedPath.empty()) return;
        bool isZip = m_downloadedPath.find(".zip") != std::string::npos;
        if (isZip) {
            if (!launchUpdaterScript()) {
                m_error = (g_lang==Lang::RU)
                    ? "Не удалось запустить скрипт обновления"
                    : "Failed to launch updater script";
                setState(State::Error);
                return;
            }
        } else {
            std::wstring wp(m_downloadedPath.begin(), m_downloadedPath.end());
            ShellExecuteW(hwnd, L"open", wp.c_str(), nullptr, nullptr, SW_SHOW);
        }
        PostQuitMessage(0); // завершаемся — bat подождёт и заменит файлы
    }

    // Вызывать каждый фрейм из ImGui
    void drawUI(bool& showWindow) {
        bool ru = (g_lang == Lang::RU);
        if (m_state == State::UpdateAvailable && !m_dialogShown)
            drawBanner(ru);
        if (!showWindow) return;

        ImGui::SetNextWindowSize({500, 0}, ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Appearing, {0.5f,0.5f});
        bool open = true;
        ImGui::Begin(ru ? "Обновление AudioRouter" : "AudioRouter Update",
                     &open, ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoScrollbar);
        if (!open) showWindow = false;

        switch (m_state) {
        case State::Idle:
            ImGui::TextDisabled(ru ? "Проверка не запускалась" : "No check run yet");
            ImGui::Spacing();
            if (ImGui::Button(ru ? "Проверить обновление" : "Check for updates", {-1,0}))
                startCheck();
            break;
        case State::Checking:
            drawSpinner(ru ? "Проверяем сервер..." : "Checking server...");
            break;
        case State::UpdateAvailable:
            m_dialogShown = true;
            drawUpdateAvailable(ru, showWindow);
            break;
        case State::Downloading:
            drawDownloading(ru);
            break;
        case State::ReadyToInstall:
            drawReadyToInstall(ru, showWindow);
            break;
        case State::UpToDate:
            ImGui::TextColored({0.3f,1.f,0.3f,1.f},
                ru ? "✓  Версия актуальна  (%s)" : "✓  Up to date  (%s)",
                UPDATER_CURRENT_VER);
            ImGui::Spacing();
            if (ImGui::Button(ru ? "Проверить снова" : "Check again", {-1,0}))
                startCheck();
            break;
        case State::Error:
            ImGui::TextColored({1.f,0.35f,0.35f,1.f}, ru ? "Ошибка:" : "Error:");
            ImGui::TextWrapped("%s", m_error.c_str());
            ImGui::Spacing();
            if (ImGui::Button(ru ? "Повторить" : "Retry", {-1,0})) startCheck();
            break;
        }
        ImGui::End();
    }

    bool        hasUpdate()  const { return m_state==State::UpdateAvailable ||
                                            m_state==State::ReadyToInstall; }
    State       state()      const { return m_state; }
    VersionInfo latestInfo() const { std::lock_guard<std::mutex> l(m_mx); return m_info; }
    ~Updater()  { joinWorker(); }

private:
    Updater() = default;
    std::atomic<State>   m_state          { State::Idle };
    std::string          m_error;
    VersionInfo          m_info;
    mutable std::mutex   m_mx;
    std::thread          m_worker;
    bool                 m_dialogShown    { false };
    std::atomic<float>   m_downloadProgress { 0.f };
    std::atomic<int64_t> m_downloadedBytes  { 0 };
    std::atomic<int64_t> m_totalBytes       { 0 };
    std::string          m_downloadedPath;

    void setState(State s) { m_state = s; }
    void joinWorker() { if (m_worker.joinable()) m_worker.join(); }

    // ── UI ────────────────────────────────────────────────────────────────────
    void drawSpinner(const char* label) {
        static int tick=0; ++tick;
        const char* s[] = {"◐","◓","◑","◒"};
        ImGui::Text("%s  %s", s[(tick/8)%4], label);
    }

    void drawBanner(bool ru) {
        auto& io = ImGui::GetIO();
        float w=380.f, h=36.f;
        ImGui::SetNextWindowPos({io.DisplaySize.x-w-12.f, io.DisplaySize.y-h-12.f}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({w,h}, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.06f,0.26f,0.06f,1.f});
        ImGui::Begin("##upd_banner", nullptr,
            ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
            ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
        { std::lock_guard<std::mutex> lk(m_mx);
          ImGui::TextColored({0.5f,1.f,0.5f,1.f},
            ru ? "↑ Доступно %s  —  Меню ▸ Справка ▸ Обновления"
               : "↑ Update %s available  —  Menu ▸ Help ▸ Updates",
            m_info.version.c_str()); }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    void drawUpdateAvailable(bool ru, bool& showWindow) {
        std::lock_guard<std::mutex> lk(m_mx);
        ImGui::TextColored({0.4f,1.f,0.5f,1.f},
            ru ? "Доступно обновление!" : "Update available!");
        ImGui::Spacing();
        ImGui::Text(ru ? "Текущая: %s" : "Current: %s", UPDATER_CURRENT_VER);
        ImGui::Text(ru ? "Новая:   %s" : "New:     %s", m_info.version.c_str());
        if (!m_info.releaseNotes.empty()) {
            ImGui::Spacing(); ImGui::Separator();
            ImGui::TextDisabled(ru ? "Что нового:" : "Release notes:");
            ImGui::BeginChild("##rn", {-1,100}, ImGuiChildFlags_Border);
            ImGui::TextWrapped("%s", m_info.releaseNotes.c_str());
            ImGui::EndChild();
        }
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        if (!m_info.downloadUrl.empty()) {
            // Кнопка автоскачивания
            ImGui::PushStyleColor(ImGuiCol_Button,        {0.08f,0.48f,0.12f,1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.12f,0.65f,0.18f,1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.06f,0.38f,0.10f,1.f});
            std::string lbl = ru
                ? ("  ⬇  Скачать и установить " + m_info.version + "  ")
                : ("  ⬇  Download & Install " + m_info.version + "  ");
            if (ImGui::Button(lbl.c_str(), {-1,0})) {
                m_mx.unlock(); startDownload(); m_mx.lock();
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(ru
                    ? "Скачает обновление и перезапустит программу"
                    : "Downloads the update and restarts automatically");
            ImGui::Spacing();
        }

        if (ImGui::Button(ru ? "  Открыть страницу релиза  "
                             : "  Open release page  ", {-1,0})) {
            std::wstring url(m_info.htmlUrl.begin(), m_info.htmlUrl.end());
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOW);
        }
        ImGui::Spacing();
        if (ImGui::Button(ru ? "Напомнить позже" : "Remind me later", {-1,0})) {
            setState(State::Idle); m_dialogShown=false; showWindow=false;
        }
    }

    void drawDownloading(bool ru) {
        float  prog = m_downloadProgress.load();
        int64_t dl  = m_downloadedBytes.load();
        int64_t tot = m_totalBytes.load();
        ImGui::TextColored({0.5f,0.8f,1.f,1.f},
            ru ? "Скачивание обновления..." : "Downloading update...");
        ImGui::Spacing();
        char ov[64];
        if (tot>0) snprintf(ov,sizeof(ov),"%.1f / %.1f MB",dl/1048576.f,tot/1048576.f);
        else        snprintf(ov,sizeof(ov),"%.1f MB",dl/1048576.f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4{0.15f,0.65f,0.20f,1.f});
        ImGui::ProgressBar(prog, {-1,22}, ov);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextDisabled(ru ? "Пожалуйста, подождите..." : "Please wait...");
    }

    void drawReadyToInstall(bool ru, bool& showWindow) {
        ImGui::TextColored({0.4f,1.f,0.5f,1.f},
            ru ? "✓  Обновление скачано!" : "✓  Update downloaded!");
        ImGui::Spacing();
        { std::lock_guard<std::mutex> lk(m_mx);
          ImGui::Text(ru ? "Версия %s готова к установке."
                         : "Version %s is ready to install.", m_info.version.c_str()); }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.28f,0.22f,0.05f,1.f});
        ImGui::BeginChild("##warn", {-1,48}, ImGuiChildFlags_Border);
        ImGui::TextColored({1.f,0.85f,0.2f,1.f},
            ru ? "⚠  Программа перезапустится для применения обновления"
               : "⚠  The app will restart to apply the update");
        ImGui::TextDisabled(ru ? "Несохранённые настройки маршрутизации будут потеряны"
                               : "Unsaved routing settings will be lost");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        {0.08f,0.48f,0.12f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.12f,0.65f,0.18f,1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.06f,0.38f,0.10f,1.f});
        if (ImGui::Button(ru ? "  ✓  Установить и перезапустить  "
                             : "  ✓  Install & Restart  ", {-1,0})) {
            HWND hwnd = (HWND)ImGui::GetMainViewport()->PlatformHandleRaw;
            applyUpdate(hwnd);
        }
        ImGui::PopStyleColor(3);
        ImGui::Spacing();
        if (ImGui::Button(ru ? "Установить позже" : "Install later", {-1,0}))
            showWindow = false;
    }

    // ── Workers ───────────────────────────────────────────────────────────────
    void workerCheck() {
        std::string body, err;
        if (!httpGetText("api.github.com",
                         "/repos/" UPDATER_OWNER "/" UPDATER_REPO "/releases/latest",
                         body, err)) {
            m_error = err; setState(State::Error);
            LOG_INFO("Updater: check error — %s", err.c_str());
            return;
        }
        VersionInfo info;
        if (!parseRelease(body, info)) {
            m_error = "Failed to parse GitHub response"; setState(State::Error); return;
        }
        LOG_INFO("Updater: latest=%s current=%s asset=%s",
                 info.version.c_str(), UPDATER_CURRENT_VER, info.downloadUrl.c_str());
        { std::lock_guard<std::mutex> lk(m_mx); m_info = info; }
        setState(isNewer(info.version, UPDATER_CURRENT_VER)
                 ? State::UpdateAvailable : State::UpToDate);
    }

    void workerDownload() {
        std::string url;
        { std::lock_guard<std::mutex> lk(m_mx); url = m_info.downloadUrl; }
        if (url.empty()) {
            m_error = "No download URL in release assets"; setState(State::Error); return;
        }

        std::string fname = url.substr(url.rfind('/')+1);
        if (fname.empty()) fname = "AudioRouter_update.zip";

        wchar_t tmpDir[MAX_PATH]; GetTempPathW(MAX_PATH, tmpDir);
        std::wstring wfname(fname.begin(), fname.end());
        std::wstring wpath = std::wstring(tmpDir) + wfname;
        // WideCharToMultiByte — правильная конвертация wstring→string
        auto wToA = [](const std::wstring& w) -> std::string {
            if (w.empty()) return {};
            int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string s(sz - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), sz, nullptr, nullptr);
            return s;
        };
        std::string  path = wToA(wpath);

        auto onProg = [this](int64_t dl, int64_t tot){
            m_downloadedBytes  = dl;
            m_totalBytes       = tot;
            m_downloadProgress = (tot>0) ? (float)dl/(float)tot : 0.f;
        };

        std::vector<uint8_t> data;
        std::string err;
        if (!httpGetBinary(url, data, err, onProg)) {
            m_error = err; setState(State::Error);
            LOG_INFO("Updater: download error — %s", err.c_str());
            return;
        }

        FILE* f = _wfopen(wpath.c_str(), L"wb");
        if (!f) { m_error = "Cannot write temp file"; setState(State::Error); return; }
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);

        LOG_INFO("Updater: saved %zu bytes → %s", data.size(), path.c_str());
        m_downloadedPath   = path;
        m_downloadProgress = 1.f;
        setState(State::ReadyToInstall);
    }

    // ── Self-update via bat script ─────────────────────────────────────────────
    bool launchUpdaterScript() {
        wchar_t exePathW[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
        std::wstring wExe = exePathW;
        std::wstring wExeDir = wExe.substr(0, wExe.rfind(L'\\'));

        wchar_t tmpDir[MAX_PATH]; GetTempPathW(MAX_PATH, tmpDir);
        std::wstring batPath = std::wstring(tmpDir) + L"ar_update.bat";

        std::wstring wZip(m_downloadedPath.begin(), m_downloadedPath.end());

        // WideCharToMultiByte — корректная конвертация путей для bat-скрипта
        auto toA = [](const std::wstring& w) -> std::string {
            if (w.empty()) return {};
            int sz = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string s(sz - 1, '\0');
            WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), sz, nullptr, nullptr);
            return s;
        };

        std::string bat;
        bat += "@echo off\r\n";
        bat += "chcp 65001 >nul 2>&1\r\n";
        bat += "title AudioRouter Updater\r\n";
        bat += "echo.\r\n";
        bat += "echo  AudioRouter Auto-Updater\r\n";
        bat += "echo  ========================\r\n";
        bat += "echo.\r\n";
        bat += "echo  Waiting for application to close...\r\n";
        bat += "timeout /t 3 /nobreak >nul\r\n";
        bat += "echo  Extracting update...\r\n";
        bat += "powershell -NoProfile -ExecutionPolicy Bypass -Command \""
               "Expand-Archive -Path '" + toA(wZip) + "'"
               " -DestinationPath '" + toA(wExeDir) + "'"
               " -Force\"\r\n";
        bat += "if %errorlevel% neq 0 (\r\n";
        bat += "  echo.\r\n";
        bat += "  echo  ERROR: Failed to extract update!\r\n";
        bat += "  echo  The zip file may be corrupted.\r\n";
        bat += "  pause\r\n";
        bat += "  exit /b 1\r\n";
        bat += ")\r\n";
        bat += "echo  Cleaning up temp files...\r\n";
        bat += "del /f /q \"" + toA(wZip) + "\"\r\n";
        bat += "echo.\r\n";
        bat += "echo  Update installed successfully!\r\n";
        bat += "echo  Starting AudioRouter...\r\n";
        bat += "timeout /t 1 /nobreak >nul\r\n";
        bat += "start \"\" \"" + toA(wExe) + "\"\r\n";
        bat += "del /f /q \"%~f0\"\r\n";   // самоудаление
        bat += "exit\r\n";

        FILE* f = _wfopen(batPath.c_str(), L"wb");
        if (!f) return false;
        fwrite(bat.c_str(), 1, bat.size(), f);
        fclose(f);

        SHELLEXECUTEINFOW sei {};
        sei.cbSize      = sizeof(sei);
        sei.fMask       = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb      = L"open";
        sei.lpFile      = L"cmd.exe";
        std::wstring args = L"/c \"" + batPath + L"\"";
        sei.lpParameters = args.c_str();
        sei.nShow       = SW_SHOW;
        return ShellExecuteExW(&sei) == TRUE;
    }

    // ── HTTP ──────────────────────────────────────────────────────────────────
    static void parseUrl(const std::string& url,
                         std::string& host, std::string& path)
    {
        size_t s = url.find("://");
        s = (s==std::string::npos) ? 0 : s+3;
        size_t sl = url.find('/', s);
        if (sl==std::string::npos){ host=url.substr(s); path="/"; }
        else { host=url.substr(s,sl-s); path=url.substr(sl); }
    }

    bool httpGetText(const std::string& host, const std::string& path,
                     std::string& outBody, std::string& outErr)
    {
        std::string url = "https://" + host + path;
        std::vector<uint8_t> data;
        if (!httpGetBinary(url, data, outErr, nullptr)) return false;
        outBody.assign((char*)data.data(), data.size());
        return true;
    }

    bool httpGetBinary(const std::string& startUrl,
                       std::vector<uint8_t>& outData,
                       std::string& outErr,
                       std::function<void(int64_t,int64_t)> onProgress)
    {
        std::string curUrl = startUrl;

        for (int redir = 0; redir < 8; ++redir) {
            std::string host, path;
            parseUrl(curUrl, host, path);

            HINTERNET hSess = WinHttpOpen(
                L"AudioRouter/" UPDATER_CURRENT_VER L" (Windows; Updater)",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!hSess) { outErr="WinHttpOpen failed"; return false; }

            std::wstring whost(host.begin(), host.end());
            HINTERNET hConn = WinHttpConnect(hSess, whost.c_str(),
                                              INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (!hConn) {
                WinHttpCloseHandle(hSess);
                outErr="WinHttpConnect failed to "+host; return false;
            }

            std::wstring wpath(path.begin(), path.end());
            HINTERNET hReq = WinHttpOpenRequest(
                hConn, L"GET", wpath.c_str(),
                nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
            if (!hReq) {
                WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
                outErr="WinHttpOpenRequest failed"; return false;
            }

            // Обрабатываем редиректы вручную (нужно для смены хоста)
            DWORD disableRedir = WINHTTP_DISABLE_REDIRECTS;
            WinHttpSetOption(hReq, WINHTTP_OPTION_DISABLE_FEATURE,
                             &disableRedir, sizeof(disableRedir));
            // Таймаут 30 сек
            DWORD timeout = 30000;
            WinHttpSetOption(hReq, WINHTTP_OPTION_RECEIVE_TIMEOUT,
                             &timeout, sizeof(timeout));

            WinHttpAddRequestHeaders(hReq,
                L"Accept: application/octet-stream, application/vnd.github+json\r\n"
                L"User-Agent: AudioRouter/" UPDATER_CURRENT_VER "\r\n",
                (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD|WINHTTP_ADDREQ_FLAG_REPLACE);

            bool sent = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS,
                                           0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                     && WinHttpReceiveResponse(hReq, nullptr);
            if (!sent) {
                DWORD e = GetLastError();
                WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
                char buf[64]; snprintf(buf,sizeof(buf),"HTTP error 0x%08X",(unsigned)e);
                outErr=buf; return false;
            }

            DWORD status=0; DWORD ssz=sizeof(status);
            WinHttpQueryHeaders(hReq,
                WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                nullptr, &status, &ssz, nullptr);

            if (status==301||status==302||status==307||status==308) {
                wchar_t loc[4096]={}; DWORD lsz=sizeof(loc);
                WinHttpQueryHeaders(hReq, WINHTTP_QUERY_LOCATION,
                                    nullptr, loc, &lsz, nullptr);
                WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
                // Redirect URL is ASCII/UTF-8 safe for GitHub URLs
                int sz = WideCharToMultiByte(CP_UTF8, 0, loc, -1, nullptr, 0, nullptr, nullptr);
                curUrl.assign(sz - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, loc, -1, curUrl.data(), sz, nullptr, nullptr);
                LOG_INFO("Updater: redirect %u → %s", status, curUrl.c_str());
                continue;
            }

            if (status != 200) {
                char buf[32]; snprintf(buf,sizeof(buf),"HTTP %u",status);
                outErr=buf;
                WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
                return false;
            }

            wchar_t clBuf[32]={}; DWORD clSz=sizeof(clBuf);
            int64_t contentLen=0;
            if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH,
                                    nullptr, clBuf, &clSz, nullptr))
                contentLen = _wtoi64(clBuf);

            outData.clear();
            if (contentLen>0) outData.reserve((size_t)contentLen);
            int64_t downloaded=0;
            DWORD avail=0;
            while (WinHttpQueryDataAvailable(hReq,&avail) && avail>0) {
                size_t prev=outData.size();
                outData.resize(prev+avail);
                DWORD rd=0;
                WinHttpReadData(hReq, outData.data()+prev, avail, &rd);
                outData.resize(prev+rd);
                downloaded+=rd;
                if (onProgress) onProgress(downloaded, contentLen);
            }
            WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
            return true;
        }
        outErr="Too many redirects"; return false;
    }

    // ── JSON / Version ────────────────────────────────────────────────────────
    static std::string jsonField(const std::string& json, const std::string& key) {
        std::string needle = "\"" + key + "\"";
        size_t pos = json.find(needle);
        if (pos==std::string::npos) return {};
        pos = json.find(':', pos);
        if (pos==std::string::npos) return {};
        while (++pos<json.size() && (json[pos]==' '||json[pos]=='\n'||json[pos]=='\r'||json[pos]=='\t'));
        if (pos>=json.size()) return {};
        if (json[pos]=='"') {
            size_t s=pos+1, e=json.find('"',s);
            while (e!=std::string::npos && json[e-1]=='\\') e=json.find('"',e+1);
            return (e!=std::string::npos) ? json.substr(s,e-s) : "";
        }
        size_t e=json.find_first_of(",}\n",pos);
        std::string v=json.substr(pos,e-pos);
        while (!v.empty()&&(v.back()==' '||v.back()=='\r'||v.back()=='\n')) v.pop_back();
        return v;
    }

    bool parseRelease(const std::string& json, VersionInfo& out) {
        out.tag=jsonField(json,"tag_name");
        out.htmlUrl=jsonField(json,"html_url");
        std::string body=jsonField(json,"body");
        std::string notes;
        for (size_t i=0;i<body.size()&&notes.size()<600;++i) {
            if (body[i]=='\\'&&i+1<body.size()) {
                char c=body[++i];
                if(c=='n') notes+='\n'; else if(c=='r'){} else if(c=='t') notes+=' '; else notes+=c;
            } else notes+=body[i];
        }
        out.releaseNotes=notes;
        size_t ap=json.find("\"assets\"");
        if (ap!=std::string::npos) {
            std::string zipU,exeU; size_t from=ap;
            while(true) {
                size_t p=json.find("\"browser_download_url\"",from);
                if(p==std::string::npos) break;
                std::string u=jsonField(json.substr(p),"browser_download_url");
                if(u.find(".zip")!=std::string::npos&&zipU.empty()) zipU=u;
                if(u.find(".exe")!=std::string::npos&&exeU.empty()) exeU=u;
                from=p+1;
            }
            out.downloadUrl=zipU.empty()?exeU:zipU;
        }
        out.version=out.tag;
        if(!out.version.empty()&&out.version[0]=='v') out.version=out.version.substr(1);
        return !out.tag.empty();
    }

    static bool isNewer(const std::string& a, const std::string& b) {
        int ma=0,mi=0,pa=0,mb=0,nb=0,pb=0;
        sscanf(a.c_str(),"%d.%d.%d",&ma,&mi,&pa);
        sscanf(b.c_str(),"%d.%d.%d",&mb,&nb,&pb);
        if(ma!=mb) return ma>mb; if(mi!=nb) return mi>nb; return pa>pb;
    }
};

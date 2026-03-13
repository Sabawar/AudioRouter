#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <shlwapi.h>
#pragma comment(lib,"shlwapi.lib")
#pragma comment(lib,"advapi32.lib")

// ============================================================================
//  AppSettings  –  сохраняется в AudioRouter.cfg рядом с .exe
// ============================================================================
struct AppSettings {
    // Последний патч
    std::string lastPatch;          // путь или пустая строка
    bool        autoLoadLastPatch = true;

    // Окно
    int  windowX      = 100;
    int  windowY      = 100;
    int  windowW      = 1440;
    int  windowH      = 880;
    bool startMinimized = false;

    // Автозапуск с Windows
    bool autostart    = false;

    // UI
    float uiScale     = 1.0f;       // 0.75 / 1.0 / 1.25 / 1.5 / 2.0
    int   language    = 0;          // 0=RU, 1=EN

    // ── Сохранение ────────────────────────────────────────────────────────
    void save(const std::string& path) const {
        FILE* f = nullptr; fopen_s(&f, path.c_str(), "w");
        if (!f) return;
        fprintf(f, "lastPatch=%s\n",      lastPatch.c_str());
        fprintf(f, "autoLoadLastPatch=%d\n", (int)autoLoadLastPatch);
        fprintf(f, "windowX=%d\n",        windowX);
        fprintf(f, "windowY=%d\n",        windowY);
        fprintf(f, "windowW=%d\n",        windowW);
        fprintf(f, "windowH=%d\n",        windowH);
        fprintf(f, "startMinimized=%d\n", (int)startMinimized);
        fprintf(f, "autostart=%d\n",      (int)autostart);
        fprintf(f, "uiScale=%.2f\n",      uiScale);
        fprintf(f, "language=%d\n",       language);
        fclose(f);
    }

    void load(const std::string& path) {
        FILE* f = nullptr; fopen_s(&f, path.c_str(), "r");
        if (!f) return;
        char key[64], val[512];
        while (fscanf_s(f, " %63[^=]=%511[^\n]\n", key, (unsigned)sizeof(key),
                        val, (unsigned)sizeof(val)) == 2)
        {
            std::string k = key, v = val;
            if      (k == "lastPatch")          lastPatch          = v;
            else if (k == "autoLoadLastPatch")  autoLoadLastPatch  = v == "1";
            else if (k == "windowX")            windowX            = std::stoi(v);
            else if (k == "windowY")            windowY            = std::stoi(v);
            else if (k == "windowW")            windowW            = std::stoi(v);
            else if (k == "windowH")            windowH            = std::stoi(v);
            else if (k == "startMinimized")     startMinimized     = v == "1";
            else if (k == "autostart")          autostart          = v == "1";
            else if (k == "uiScale")            uiScale            = std::stof(v);
            else if (k == "language")           language           = std::stoi(v);
        }
        fclose(f);
    }

    // ── Автозапуск Windows ────────────────────────────────────────────────
    static std::wstring exePath() {
        wchar_t p[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        return p;
    }

    void applyAutostart() const {
        HKEY hk;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) return;

        const wchar_t* name = L"AudioRouter";
        if (autostart) {
            // Add /minimized flag so the app starts to tray
            std::wstring val = L"\"" + exePath() + L"\" /minimized";
            RegSetValueExW(hk, name, 0, REG_SZ,
                           (const BYTE*)val.c_str(),
                           (DWORD)((val.size()+1)*sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hk, name);
        }
        RegCloseKey(hk);
    }

    static bool isAutostartEnabled() {
        HKEY hk;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS) return false;
        bool exists = (RegQueryValueExW(hk, L"AudioRouter",
                                        nullptr, nullptr, nullptr, nullptr)
                       == ERROR_SUCCESS);
        RegCloseKey(hk);
        return exists;
    }
};

// Global instance
inline AppSettings g_settings;

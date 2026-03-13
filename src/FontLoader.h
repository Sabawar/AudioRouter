#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "imgui.h"
#include "Logger.h"
#include <string>

// ============================================================================
//  FontLoader
//  Пробует загрузить системный шрифт с поддержкой кириллицы.
//  Порядок попыток: Segoe UI → Arial → Tahoma → встроенный ImGui (ASCII)
// ============================================================================
namespace FontLoader {

inline ImFont* loadCyrillic(float sizePx = 15.0f) {
    ImGuiIO& io = ImGui::GetIO();

    // Use built-in Cyrillic range (Latin + Cyrillic), then merge extra symbols
    const ImWchar* cyrRange = io.Fonts->GetGlyphRangesCyrillic();

    // Extra ranges: geometric shapes ●○□■▶▼ etc + arrows + misc symbols
    static const ImWchar extraRanges[] = {
        0x2013, 0x2014,   // en-dash, em-dash (–—)
        0x2019, 0x2019,   // right single quote
        0x25A0, 0x25FF,   // Geometric Shapes (●○▶▼■□ etc.)
        0x2190, 0x21FF,   // Arrows
        0,
    };

    const char* candidates[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
        "C:\\Windows\\Fonts\\verdana.ttf",
        nullptr
    };

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = false;

    ImFont* mainFont = nullptr;

    for (int i = 0; candidates[i]; ++i) {
        DWORD attr = GetFileAttributesA(candidates[i]);
        if (attr == INVALID_FILE_ATTRIBUTES) continue;

        // Load main font with Cyrillic
        mainFont = io.Fonts->AddFontFromFileTTF(candidates[i], sizePx, &cfg, cyrRange);
        if (!mainFont) continue;

        // Merge extra symbols from same font
        ImFontConfig mergeCfg;
        mergeCfg.MergeMode   = true;
        mergeCfg.OversampleH = 2;
        mergeCfg.OversampleV = 2;
        io.Fonts->AddFontFromFileTTF(candidates[i], sizePx, &mergeCfg, extraRanges);

        LOG_INFO("Font: loaded '%s' %.0fpx with Cyrillic+symbols", candidates[i], sizePx);
        return mainFont;
    }

    LOG_WARN("Font: no system TTF found, using ImGui default (no Cyrillic)");
    return io.Fonts->AddFontDefault();
}

} // namespace FontLoader

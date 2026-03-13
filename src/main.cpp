//=============================================================================
//  AudioRouter  -  main.cpp
//=============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // NOTIFYICONDATA, Shell_NotifyIcon
#include <d3d11.h>
#include <dxgi.h>
#include <tchar.h>
#include <shlwapi.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "Logger.h"
#include "CrashHandler.h"
#include "CpuInfo.h"
#include "AudioMath.h"
#include "FontLoader.h"
#include "Lang.h"
#include "AppSettings.h"
#include "RoutingGraph.h"
#include "AudioEngine.h"
#include "UI/NodeEditorUI.h"

#define IDI_APP      101
#define WM_TRAYICON  (WM_USER + 1)
#define ID_TRAY_SHOW  2001
#define ID_TRAY_EXIT  2002

// ─── Globals ──────────────────────────────────────────────────────────────────
static ID3D11Device*            g_pd3dDevice           = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext    = nullptr;
static IDXGISwapChain*          g_pSwapChain           = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static RoutingGraph  g_graph;
static AudioEngine   g_engine;
static NodeEditorUI  g_ui;
bool                 g_showLog = false;   // open manually via menu

static HWND          g_hwnd    = nullptr;
static HICON         g_hIcon   = nullptr;
static bool          g_minimizedToTray = false;
static NOTIFYICONDATAW g_nid   = {};

std::string g_settingsPath;   // exported for drawSettingsWindow
std::string g_patchPath;

// ─── Helpers ──────────────────────────────────────────────────────────────────
static std::wstring getExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}
static std::string wToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n-1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ─── Tray ─────────────────────────────────────────────────────────────────────
static void trayAdd(HWND hwnd, HICON icon) {
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = icon;
    wcscpy_s(g_nid.szTip, L"AudioRouter");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void trayRemove() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}
static std::wstring utf8ToWide(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

static void trayShowMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    std::wstring showW = utf8ToWide((g_lang == Lang::RU) ? "Показать окно" : "Show Window");
    std::wstring exitW = utf8ToWide((g_lang == Lang::RU) ? "Выход"        : "Exit");
    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, showW.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, exitW.c_str());
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}
static void minimizeToTray(HWND hwnd) {
    ShowWindow(hwnd, SW_HIDE);
    g_minimizedToTray = true;
    LOG_INFO("Minimized to tray");
}
static void restoreFromTray(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    g_minimizedToTray = false;
    LOG_INFO("Restored from tray");
}

// ─── D3D11 ────────────────────────────────────────────────────────────────────
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd {};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hWnd;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, lvls, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (FAILED(hr)) {
        LOG_WARN("Hardware D3D11 failed (0x%08X), trying WARP...", (unsigned)hr);
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            0, lvls, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
        if (FAILED(hr)) { LOG_FATAL("WARP also failed: 0x%08X", (unsigned)hr); return false; }
        LOG_WARN("Using WARP software renderer");
    } else {
        LOG_INFO("D3D11 hardware device OK (feature level 0x%04X)", (unsigned)fl);
    }
    return true;
}
void CreateRenderTarget() {
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
        g_pd3dDevice->CreateRenderTargetView(bb, nullptr, &g_mainRenderTargetView);
        bb->Release();
    }
}
void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}
void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

// ─── WndProc ──────────────────────────────────────────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        if (wParam == SIZE_MINIMIZED)
            minimizeToTray(hWnd);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
        if ((wParam & 0xFFF0) == SC_MINIMIZE) { minimizeToTray(hWnd); return 0; }
        break;
    case WM_GETMINMAXINFO: {
        // Minimum window size so columns don't collapse
        MINMAXINFO* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize = {700, 400};
        return 0;
    }
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) restoreFromTray(hWnd);
        if (lParam == WM_RBUTTONUP)     trayShowMenu(hWnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_SHOW) restoreFromTray(hWnd);
        if (LOWORD(wParam) == ID_TRAY_EXIT) { trayRemove(); PostQuitMessage(0); }
        return 0;
    case WM_CLOSE:
        minimizeToTray(hWnd);   // Close button → tray, not exit
        return 0;
    case WM_DESTROY:
        LOG_INFO("WM_DESTROY");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─── WinMain ──────────────────────────────────────────────────────────────────
int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE,
                    _In_ LPWSTR lpCmdLine, _In_ int)
{
    // ── Single-instance guard ─────────────────────────────────────────────
    // Named mutex is unique per user session; second launch detects it.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE,
                                  L"AudioRouter_SingleInstance_Mutex_v1");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Find the existing window and bring it to front
        HWND existing = FindWindowW(L"AudioRouterClass", nullptr);
        if (existing) {
            if (IsIconic(existing))
                ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        } else {
            MessageBoxW(nullptr,
                L"AudioRouter уже запущен.\nПроверьте панель задач или системный трей.",
                L"AudioRouter",
                MB_OK | MB_ICONINFORMATION);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }
    // Keep mutex open for the lifetime of this process

    std::wstring exeDir  = getExeDir();
    std::wstring logPathW  = exeDir + L"\\AudioRouter.log";
    std::wstring cfgPathW  = exeDir + L"\\AudioRouter.cfg";
    std::wstring patchPathW= exeDir + L"\\AudioRouter.patch";

    g_settingsPath = wToUtf8(cfgPathW);
    g_patchPath    = wToUtf8(patchPathW);

    Logger::get().open(wToUtf8(logPathW));
    CrashHandler::install(logPathW);

    LOG_INFO("=== AudioRouter starting ===");
    LOG_INFO("EXE dir: %s", wToUtf8(exeDir).c_str());

    // ── Load settings ─────────────────────────────────────────────────────
    g_settings.load(g_settingsPath);
    g_lang = (Lang)g_settings.language;
    LOG_INFO("Settings loaded: scale=%.2f lang=%d autostart=%d",
             g_settings.uiScale, g_settings.language, (int)g_settings.autostart);

    // ── Check /minimized command line ──────────────────────────────────────
    bool startMinimized = g_settings.startMinimized ||
                          (wcsstr(lpCmdLine, L"/minimized") != nullptr);

    // ── CPU detection + AudioMath ──────────────────────────────────────────
    const auto& cpu = CpuInfo::get();
    LOG_INFO("CPU: %s", cpu.brand);
    LOG_INFO("CPU features: SSE2=%d AVX=%d AVX2=%d FMA=%d",
             cpu.sse2, cpu.avx, cpu.avx2, cpu.fma);
    AudioMath::init();
    LOG_INFO("AudioMath: %s", AudioMath::activePath());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LOG_HR("CoInitializeEx", hr);

    // ── Window ────────────────────────────────────────────────────────────
    g_hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP),
                                IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (!g_hIcon) {
        LOG_WARN("Icon resource not found, using default");
        g_hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    }

    WNDCLASSEXW wc {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = g_hIcon;
    wc.hIconSm       = g_hIcon;
    wc.lpszClassName = L"AudioRouterClass";
    RegisterClassExW(&wc);

    int wx = g_settings.windowX, wy = g_settings.windowY;
    int ww = g_settings.windowW, wh = g_settings.windowH;

    g_hwnd = CreateWindowExW(0, wc.lpszClassName,
                              L"AudioRouter  \x2013  Professional Audio Routing",
                              WS_OVERLAPPEDWINDOW,
                              wx, wy, ww, wh,
                              nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) { LOG_FATAL("CreateWindowExW failed"); return 1; }
    LOG_INFO("Window created %dx%d at (%d,%d)", ww, wh, wx, wy);

    if (!CreateDeviceD3D(g_hwnd)) {
        MessageBoxW(g_hwnd, L"Failed to create D3D11 device.\nCheck AudioRouter.log.",
                    L"AudioRouter - Fatal Error", MB_ICONERROR);
        return 1;
    }
    CreateRenderTarget();

    // Add tray icon (always — allows restore even if started minimized)
    trayAdd(g_hwnd, g_hIcon);

    if (startMinimized) {
        // Don't show window, go straight to tray
        g_minimizedToTray = true;
        LOG_INFO("Started minimized to tray");
    } else {
        ShowWindow(g_hwnd, SW_SHOWDEFAULT);
        UpdateWindow(g_hwnd);
    }

    // ── ImGui ─────────────────────────────────────────────────────────────
    LOG_INFO("Initialising ImGui %s", IMGUI_VERSION);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    static std::string iniPath = wToUtf8(exeDir) + "\\AudioRouter.ini";
    io.IniFilename = iniPath.c_str();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;
    style.Colors[ImGuiCol_WindowBg]        = {0.12f,0.12f,0.13f,1.0f};
    style.Colors[ImGuiCol_TitleBg]         = {0.08f,0.08f,0.09f,1.0f};
    style.Colors[ImGuiCol_TitleBgActive]   = {0.15f,0.25f,0.40f,1.0f};
    style.Colors[ImGuiCol_Header]          = {0.20f,0.30f,0.50f,0.60f};
    style.Colors[ImGuiCol_HeaderHovered]   = {0.25f,0.38f,0.60f,0.80f};
    style.Colors[ImGuiCol_Button]          = {0.20f,0.30f,0.50f,0.80f};
    style.Colors[ImGuiCol_ButtonHovered]   = {0.30f,0.45f,0.70f,1.00f};
    style.Colors[ImGuiCol_FrameBg]         = {0.18f,0.18f,0.20f,1.00f};
    style.Colors[ImGuiCol_CheckMark]       = {0.30f,0.90f,0.40f,1.00f};
    style.Colors[ImGuiCol_SliderGrab]      = {0.30f,0.55f,0.85f,1.00f};

    // Font size scales with UI scale setting
    float baseFontPx = 15.0f * g_settings.uiScale;
    FontLoader::loadCyrillic(baseFontPx);
    io.FontGlobalScale = 1.0f;

    // Build atlas now so we can validate before the first frame
    io.Fonts->Build();
    LOG_INFO("Font atlas built: %d glyphs in default font",
             io.Fonts->Fonts.Size > 0 ? io.Fonts->Fonts[0]->Glyphs.Size : 0);  // scale is baked into font size

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    LOG_INFO("ImGui backends OK");

    // ── Audio engine ──────────────────────────────────────────────────────
    LOG_INFO("Initialising AudioEngine...");
    if (!g_engine.init(&g_graph)) {
        LOG_ERROR("AudioEngine::init failed");
        MessageBoxW(g_hwnd,
            L"Audio engine init failed.\nCheck AudioRouter.log.",
            L"AudioRouter - Warning", MB_ICONWARNING);
    } else {
        LOG_INFO("AudioEngine OK");
    }

    g_ui.init(&g_graph, &g_engine);

    // ── Auto-load last patch ───────────────────────────────────────────────
    if (g_settings.autoLoadLastPatch) {
        std::string patch = g_settings.lastPatch.empty()
                            ? g_patchPath
                            : g_settings.lastPatch;
        if (GetFileAttributesA(patch.c_str()) != INVALID_FILE_ATTRIBUTES) {
            LOG_INFO("Auto-loading patch: %s", patch.c_str());
            g_graph.loadFromFile(patch);
        }
    }

    LOG_INFO("Entering main loop");

    // ── Main loop ─────────────────────────────────────────────────────────
    const ImVec4 clearColor = {0.10f, 0.10f, 0.11f, 1.00f};
    MSG msg {};
    int frameCount = 0;
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // If minimized to tray, sleep and don't render
        if (g_minimizedToTray) {
            Sleep(50);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (frameCount == 0) LOG_INFO("First frame OK");

        g_ui.renderFrame(g_hwnd);
        Logger::get().drawWindow(&g_showLog);

        ImGui::Render();
        const float cc[4] = { clearColor.x, clearColor.y, clearColor.z, clearColor.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
        ++frameCount;
    }

    // ── Save state on exit ────────────────────────────────────────────────
    LOG_INFO("Saving settings on exit...");

    // Save window rect
    RECT rc;
    if (GetWindowRect(g_hwnd, &rc)) {
        g_settings.windowX = rc.left;
        g_settings.windowY = rc.top;
        g_settings.windowW = rc.right  - rc.left;
        g_settings.windowH = rc.bottom - rc.top;
    }

    // Save last patch
    if (GetFileAttributesA(g_patchPath.c_str()) != INVALID_FILE_ATTRIBUTES ||
        g_graph.nodes().size() > 0) {
        g_graph.saveToFile(g_patchPath);
        g_settings.lastPatch = g_patchPath;
        LOG_INFO("Auto-saved patch: %s", g_patchPath.c_str());
    }

    g_settings.language = (int)g_lang;
    g_settings.save(g_settingsPath);
    LOG_INFO("Settings saved");

    // ── Cleanup ───────────────────────────────────────────────────────────
    g_engine.shutdown();
    g_ui.shutdown();
    trayRemove();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    CoUninitialize();

    LOG_INFO("=== AudioRouter exited cleanly ===");
    Logger::get().close();
    return 0;
}

/**
 * YM7129 Drum Pad - ImGui + DX11
 *
 * 12 drum pads (6 types x L/H), mouse + keyboard.
 * Connect/Disconnect/Reset. Delay slider 0-200us.
 * Uses verified 3-byte raw protocol: [slot, 0x80, addr] [slot, 0x81, data]
 */

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <tchar.h>

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <string>

extern "C" {
#include "spfm.h"
#include "ym7129.h"
}

// ============================================================
// Drum pad definitions
// ============================================================
typedef struct {
    const char *name;
    uint8_t    rhy_type;
    uint8_t    lh;
    uint8_t    channel;
} drum_pad_t;

static const drum_pad_t DRUM_PADS[12] = {
    { "BD  H",  0, 0, 0 }, { "BD  L",  0, 1, 0 },
    { "SD  H",  1, 0, 0 }, { "SD  L",  1, 1, 0 },
    { "C.HH H", 2, 0, 0 }, { "C.HH L", 2, 1, 0 },
    { "O.HH H", 3, 0, 0 }, { "O.HH L", 3, 1, 0 },
    { "Tom H",  4, 0, 0 }, { "Tom L",  4, 1, 0 },
    { "Ride H", 5, 0, 0 }, { "Ride L", 5, 1, 0 },
};

// VK mapping: 0., 12, 45, 78, */-, +  (6 groups x 2 keys)
static const int PAD_VK[12] = {
    VK_NUMPAD0, 0x6E, /* VK_DECIMAL */
    VK_NUMPAD1, VK_NUMPAD2,
    VK_NUMPAD4, VK_NUMPAD5,
    VK_NUMPAD7, VK_NUMPAD8,
    VK_MULTIPLY, VK_SUBTRACT,
    VK_DIVIDE,   VK_ADD,
};

static const char *PAD_KEYNAMES[12] = {
    "0.",".","1","2","4","5","7","8","*","-","/","+"
};

static void on_key_down(int vk); // forward declaration, defined after strike_drum

// ============================================================
// State
// ============================================================
static bool    g_connected = false;
static uint8_t g_slot = 0;
static int     g_volume = 0;
static int     g_delay_us = 80;
static float   g_hitAnim[12] = {};

// ============================================================
// Log component (ported from DMPlayer ym2163)
// ============================================================
static std::string g_logBuffer;
static bool        g_logAutoScroll = true;
static bool        g_logScrollToBottom = false;
static size_t      g_logLastSize = 0;
static char        g_logDisplay[65536] = "";
static bool        g_logExpanded = false;

static void log_cmd(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_logBuffer += buf;
    g_logBuffer += "\n";
    if (g_logBuffer.size() > 64000)
        g_logBuffer = g_logBuffer.substr(g_logBuffer.size() - 32000);
    g_logScrollToBottom = true;
}

// ============================================================
// DX11 globals
// ============================================================
static ID3D11Device*            g_pd3dDevice = NULL;
static ID3D11DeviceContext*     g_pd3dDeviceContext = NULL;
static IDXGISwapChain*          g_pSwapChain = NULL;
static ID3D11RenderTargetView*  g_mainRenderTargetView = NULL;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
            levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

// ============================================================
// Win32 message handler
// ============================================================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static HWND g_hWnd = NULL;

// Forward declaration
static void on_key_down(int vk);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        on_key_down((int)wParam);
        // Still pass to ImGui for docking/navigation
    }
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ============================================================
// Drum actions
// ============================================================
static void strike_drum(int idx) {
    if (!g_connected || idx < 0 || idx >= 12) return;
    const drum_pad_t *p = &DRUM_PADS[idx];
    // Re-init chip before each strike (same as ym7129_drum_all.c)
    ym7129_init(g_slot);
    spfm_flush();
    log_cmd("Strike: %s (Rhy=%d L/H=%d Vol=%d)", p->name, p->rhy_type, p->lh, g_volume);
    ym7129_drum_strike(g_slot, p->channel, p->rhy_type, p->lh, (uint8_t)g_volume);
    g_hitAnim[idx] = 1.0f;
}

static void on_key_down(int vk) {
    if (!g_connected) return;
    for (int i = 0; i < 12; i++) {
        if (vk == PAD_VK[i]) { strike_drum(i); return; }
    }
    if (vk == VK_OEM_4) // [ bracket
        g_volume = (g_volume > 0) ? g_volume - 1 : 0;
    if (vk == VK_OEM_6) // ] bracket
        g_volume = (g_volume < 15) ? g_volume + 1 : 15;
}

static void do_connect() {
    if (g_connected) return;
    if (spfm_init(0) != 0) {
        log_cmd("ERROR: Cannot open SPFM device!");
        MessageBox(g_hWnd, "Cannot open SPFM device!", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    g_connected = true;
    ym7129_set_write_delay((uint32_t)g_delay_us);
    ym7129_init(g_slot);
    Sleep(100);
    log_cmd("Connected (delay=%dus)", g_delay_us);
}

static void do_disconnect() {
    if (!g_connected) return;
    ym7129_drum_mute(g_slot);
    spfm_flush();
    Sleep(50);
    spfm_cleanup();
    g_connected = false;
    log_cmd("Disconnected");
}

static void do_reset() {
    if (!g_connected) return;
    ym7129_mute(g_slot);
    ym7129_drum_mute(g_slot);
    Sleep(50);
    ym7129_init(g_slot);
    log_cmd("Reset");
}

// ============================================================
// ImGui UI - drum pad window (rendered inside DockSpace)
// ============================================================
static void RenderDrumPad() {
    ImGui::SetNextWindowSize(ImVec2(380, 520), ImGuiCond_FirstUseEver);
    ImGui::Begin("Drum Pad");

    // Status
    if (g_connected)
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Connected");
    else
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Disconnected");

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 140);
    ImGui::Text("Vol: %d   Delay: %dus", g_volume, g_delay_us);
    ImGui::Spacing();

    // Connect / Disconnect / Reset
    float btnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    ImGui::BeginDisabled(g_connected);
    if (ImGui::Button("Connect", ImVec2(btnW, 0))) do_connect();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_connected);
    if (ImGui::Button("Disconnect", ImVec2(btnW, 0))) do_disconnect();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_connected);
    if (ImGui::Button("Reset", ImVec2(btnW, 0))) do_reset();
    ImGui::EndDisabled();

    ImGui::Spacing();

    // Delay slider
    ImGui::Text("Write Delay (us):");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##delay", &g_delay_us, 0, 200)) {
        if (g_connected) ym7129_set_write_delay((uint32_t)g_delay_us);
    }

    ImGui::Spacing();

    // Drum pads: 2 columns x 6 rows
    float padW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    float padH = 42.0f;

    for (int row = 0; row < 6; row++) {
        for (int col = 0; col < 2; col++) {
            int i = row * 2 + col;
            ImGui::PushID(i);

            if (g_hitAnim[i] > 0.0f) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.25f, 0.35f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.35f, 0.5f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            }

            if (col > 0) ImGui::SameLine();
            char label[32];
            snprintf(label, sizeof(label), "%s [%s]", DRUM_PADS[i].name, PAD_KEYNAMES[i]);
            if (ImGui::Button(label, ImVec2(padW, padH))) strike_drum(i);

            ImGui::PopStyleColor(3);
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("[ ] = Volume   Keys: Num0-5,7,8,Num*,Num-,Num/,Num+");
    ImGui::End();
}

// ============================================================
// Log window (ported from DMPlayer gui_renderer_impl.cpp)
// ============================================================
static void RenderLog() {
    ImGui::SetNextWindowSize(ImVec2(380, 200), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Log")) { ImGui::End(); return; }

    if (ImGui::CollapsingHeader("Log", g_logExpanded ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        g_logExpanded = true;
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &g_logAutoScroll);
        ImGui::SameLine();
        if (ImGui::Button("Clear##Log")) {
            g_logBuffer.clear();
            g_logDisplay[0] = '\0';
            g_logLastSize = 0;
        }

        size_t copyLen = (g_logBuffer.length() < sizeof(g_logDisplay) - 1) ?
                         g_logBuffer.length() : sizeof(g_logDisplay) - 1;
        memcpy(g_logDisplay, g_logBuffer.c_str(), copyLen);
        g_logDisplay[copyLen] = '\0';

        bool log_changed = (g_logBuffer.length() != g_logLastSize);
        g_logLastSize = g_logBuffer.length();
        if (g_logAutoScroll && log_changed) g_logScrollToBottom = true;

        float logHeight = 150;
        ImGui::BeginChild("LogScrollRegion", ImVec2(0, logHeight), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImVec2 text_size = ImGui::CalcTextSize(g_logDisplay, NULL, false, -1.0f);
        float line_height = ImGui::GetTextLineHeightWithSpacing();
        float min_visible_height = ImGui::GetContentRegionAvail().y;
        float input_height = (text_size.y > min_visible_height) ? text_size.y + line_height * 2 : min_visible_height;
        ImGui::InputTextMultiline("##LogText", g_logDisplay, sizeof(g_logDisplay),
            ImVec2(-1, input_height), ImGuiInputTextFlags_ReadOnly);
        if (g_logScrollToBottom) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
            g_logScrollToBottom = false;
        }
        ImGui::EndChild();
    } else {
        g_logExpanded = false;
    }

    ImGui::End();
}

// ============================================================
// WinMain
// ============================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Set DPI awareness (Per Monitor V2) - like DMPlayer
    HMODULE user32 = LoadLibraryA("user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(void*);
        SetProcessDpiAwarenessContext_t func =
            (SetProcessDpiAwarenessContext_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (func) {
            func((void*)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        } else {
            HMODULE shcore = LoadLibraryA("shcore.dll");
            if (shcore) {
                typedef HRESULT (WINAPI *SetProcessDpiAwareness_t)(int);
                SetProcessDpiAwareness_t func2 =
                    (SetProcessDpiAwareness_t)GetProcAddress(shcore, "SetProcessDpiAwareness");
                if (func2) func2(2); // PROCESS_PER_MONITOR_DPI_AWARE
                FreeLibrary(shcore);
            }
        }
        FreeLibrary(user32);
    }
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "YM7129DrumPad", NULL };
    RegisterClassEx(&wc);

    g_hWnd = CreateWindowExA(0, wc.lpszClassName, "YM7129 Drum Pad",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        NULL, NULL, wc.hInstance, NULL);

    if (!CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    // Save imgui.ini next to executable for reliable layout persistence
    {
        wchar_t wbuf[MAX_PATH];
        GetModuleFileNameW(NULL, wbuf, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(wbuf, L'\\');
        if (lastSlash) { wcscpy(lastSlash + 1, L"imgui.ini"); }
        char inipath[MAX_PATH];
        snprintf(inipath, sizeof(inipath), "%S", wbuf);
        io.IniFilename = strdup(inipath);
    }
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable
                    | ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoDecoration = false;
    io.FontGlobalScale = 1.0f;
    io.FontAllowUserScaling = false;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.AntiAliasedLines = false;
    style.AntiAliasedLinesUseTex = false;
    style.AntiAliasedFill = false;
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // HiDPI font: msyh.ttc with oversample + PixelSnapH
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 1;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 20.0f, &fontConfig,
        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (!font)
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simsun.ttc", 20.0f, &fontConfig,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (!font) {
        fontConfig.SizePixels = 20.0f;
        io.Fonts->AddFontDefault(&fontConfig);
    }

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    static bool s_dockLayoutInit = false;
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) break;
        }
        if (msg.message == WM_QUIT) break;

        // Handle swap chain occlusion (minimized)
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10); continue;
        }
        g_SwapChainOccluded = false;

        // Handle resize
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Decay hit animations
        float dt = io.DeltaTime;
        for (int i = 0; i < 12; i++) {
            if (g_hitAnim[i] > 0.0f) {
                g_hitAnim[i] -= dt * 6.0f;
                if (g_hitAnim[i] < 0.0f) g_hitAnim[i] = 0.0f;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // DockSpace host (full viewport, like DMPlayer)
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##DockSpaceHost", nullptr,
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus);
        ImGui::PopStyleVar(3);

        // Menu bar
        if (ImGui::BeginMenuBar()) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "YM7129 Drum Pad");
            ImGui::EndMenuBar();
        }

        ImGuiID dockID = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockID, ImVec2(0, 0), ImGuiDockNodeFlags_None);

        // Initial layout: center child window
        if (!s_dockLayoutInit) {
            s_dockLayoutInit = true;
            ImGui::DockBuilderRemoveNode(dockID);
            ImGui::DockBuilderAddNode(dockID, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodePos(dockID, viewport->WorkPos);
            ImGui::DockBuilderSetNodeSize(dockID, viewport->WorkSize);
            ImGui::DockBuilderFinish(dockID);
        }

        // Render child windows
        RenderDrumPad();
        RenderLog();

        ImGui::End(); // ##DockSpaceHost

        ImGui::Render();
        const float clear_color[4] = { 0.12f, 0.12f, 0.16f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Multi-viewport rendering
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    if (g_connected) { ym7129_drum_mute(g_slot); spfm_flush(); spfm_cleanup(); }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hWnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return 0;
}

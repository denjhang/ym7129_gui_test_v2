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
static void on_key_up(int vk);   // forward declaration

// ============================================================
// State
// ============================================================
static bool    g_connected = false;
static uint8_t g_slot = 0;
static int     g_volume = 0;
static bool    g_keyStates[256] = {};
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
// Piano keyboard (from DMPlayer YM2163 key mapping)
// ============================================================
typedef struct { int vk; int note; int octave_offset; } KeyMapping;
static const KeyMapping g_pianoKeyMappings[] = {
    {'Z', 0, 0}, {'X', 2, 0}, {'C', 4, 0}, {'V', 5, 0}, {'B', 7, 0}, {'N', 9, 0}, {'M', 11, 0},
    {VK_OEM_COMMA, 0, 1}, {VK_OEM_PERIOD, 2, 1}, {VK_OEM_2, 4, 1},
    {'S', 1, 0}, {'D', 3, 0}, {'G', 6, 0}, {'H', 8, 0}, {'J', 10, 0}, {'K', 1, 1}, {'L', 3, 1},
    {'Q', 0, 1}, {'W', 2, 1}, {'E', 4, 1}, {'R', 5, 1}, {'T', 7, 1}, {'Y', 9, 1}, {'U', 11, 1},
    {'I', 0, 2}, {'O', 2, 2}, {'P', 4, 2}, {VK_OEM_4, 5, 2}, {VK_OEM_6, 7, 2},
    {'2', 1, 1}, {'3', 3, 1}, {'5', 6, 1}, {'6', 8, 1}, {'7', 10, 1},
    {'8', 1, 2}, {'9', 3, 2}, {'0', 6, 2},
};
static const int g_numPianoKeys = sizeof(g_pianoKeyMappings) / sizeof(KeyMapping);
static const bool g_isBlackNote[12] = {false, true, false, true, false, false, true, false, true, false, true, false};

static int   g_currentOctave = 2; // Z=C4 (same as DMPlayer)
static int   g_pianoChPlaying[128] = {};  // indexed by MIDI note, -1=none
static float g_pianoLevel[128] = {};

// Editable FM patch (start with simple sine-like tone)
static ym7129_patch_t g_fmPatch = {
    0x60, // feedback_lr: L+R, FB=0
    0x00, // m_mlws: KON=0, ML=0
    0xFF, // m_tl: mute
    0x00, // m_ardr
    0x00, // m_slrr
    0x0D, // c_mlws: KON=0, ML=13
    0x30, // c_tl: volume
    0x58, // c_ardr: AR=5, DR=8
    0x0B, // c_slrr: SL=0, RR=11
};

static void reload_fm_patch() {
    if (!g_connected) return;
    for (int ch = 0; ch < 8; ch++)
        ym7129_load_patch(g_slot, ch, &g_fmPatch);
}

static void piano_key_down(int note, int octave) {
    if (!g_connected) return;
    int midi_note = (octave + 2) * 12 + note;
    int ch = ym7129_play_note(g_slot, midi_note, 100);
    if (midi_note >= 0 && midi_note < 128) {
        g_pianoChPlaying[midi_note] = ch;
        g_pianoLevel[midi_note] = 1.0f; // will be held at 1.0 while key is pressed
    }
    log_cmd("Piano ON: ch=%d midi=%d", ch, midi_note);
}

static void piano_key_up(int note, int octave) {
    if (!g_connected) return;
    int midi_note = (octave + 2) * 12 + note;
    // Don't clear visual state here — let the decay loop fade out
    // (same as DMPlayer: UpdateChannelLevels handles fadeout)
    ym7129_stop_note(g_slot, midi_note);
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

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        on_key_down((int)wParam);
    } else if (msg == WM_KEYUP) {
        on_key_up((int)wParam);
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
    if (g_keyStates[vk]) return; // ignore auto-repeat (same as DMPlayer)
    g_keyStates[vk] = true;
    // Drum pads (numpad)
    for (int i = 0; i < 12; i++) {
        if (vk == PAD_VK[i]) { strike_drum(i); return; }
    }
    // Piano keys (main keyboard)
    for (int i = 0; i < g_numPianoKeys; i++) {
        if (vk == g_pianoKeyMappings[i].vk) {
            int note = g_pianoKeyMappings[i].note;
            int octave = g_currentOctave + g_pianoKeyMappings[i].octave_offset;
            piano_key_down(note, octave);
            return;
        }
    }
    // Octave switch
    if (vk == VK_PRIOR && g_currentOctave < 5) { g_currentOctave++; log_cmd("Octave: %d", g_currentOctave); }
    if (vk == VK_NEXT && g_currentOctave > 0) { g_currentOctave--; log_cmd("Octave: %d", g_currentOctave); }
}

static void on_key_up(int vk) {
    g_keyStates[vk] = false;
    if (!g_connected) return;
    for (int i = 0; i < g_numPianoKeys; i++) {
        if (vk == g_pianoKeyMappings[i].vk) {
            int note = g_pianoKeyMappings[i].note;
            int octave = g_currentOctave + g_pianoKeyMappings[i].octave_offset;
            piano_key_up(note, octave);
            return;
        }
    }
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
    // Load default FM patch on all 8 channels
    for (int ch = 0; ch < 8; ch++)
        ym7129_load_patch(g_slot, ch, &g_fmPatch);
    spfm_flush();
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
// Piano keyboard window (from smaf_window.cpp RenderPianoArea)
// ============================================================
static void RenderPiano() {
    ImGui::SetNextWindowSize(ImVec2(500, 170), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Piano")) { ImGui::End(); return; }

    ImGui::Text("Octave: %d  (PageUp/PageDown)", g_currentOctave);
    ImGui::SameLine();
    ImGui::TextDisabled("Keys: Z-M S-L Q-U I-P  2-3 5-7 9-0");

    ImGui::Spacing();

    ImGui::BeginChild("PianoScroll", ImVec2(0, 110), false, ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();

    const int kMinNote = 36; // C2
    const int kMaxNote = 96; // B7

    float whiteW = 20.0f, whiteH = 100.0f;
    float blackW = 12.0f, blackH = 60.0f;

    // Count white keys
    int numWhiteKeys = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (!g_isBlackNote[n % 12]) numWhiteKeys++;
    }

    float pianoWidth = numWhiteKeys * whiteW;
    float availWidth = ImGui::GetContentRegionAvail().x;
    float centerOffset = (availWidth > pianoWidth) ? (availWidth - pianoWidth) * 0.5f : 0.0f;

    // Base color
    static const ImVec4 g_pianoColor = ImVec4(0.0f, 1.0f, 0.5f, 1.0f);

    auto blendKey = [&](int midiNote, bool isBlack) -> ImU32 {
        if (midiNote < 0 || midiNote >= 128 || g_pianoLevel[midiNote] <= 0.0f) {
            return isBlack ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
        }
        float lv = powf(g_pianoLevel[midiNote], 0.5f);
        float baseMul = isBlack ? (0.1f + 0.7f * lv) : (0.15f + 0.85f * lv);
        int r = (int)(g_pianoColor.x * 255 * baseMul);
        int g = (int)(g_pianoColor.y * 255 * baseMul);
        int b = (int)(g_pianoColor.z * 255 * baseMul);
        return IM_COL32(r, g, b, 255);
    };

    // Pass 1: white keys
    int wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (g_isBlackNote[n % 12]) continue;
        float x = p.x + centerOffset + wkIdx * whiteW;
        ImU32 fillCol = blendKey(n, false);
        draw_list->AddRectFilled(ImVec2(x, p.y), ImVec2(x + whiteW - 1, p.y + whiteH), fillCol);
        draw_list->AddRect(ImVec2(x, p.y), ImVec2(x + whiteW, p.y + whiteH), IM_COL32(80, 80, 80, 255));
        if (n % 12 == 0) {
            char buf[8]; snprintf(buf, sizeof(buf), "C%d", (n / 12) - 1);
            draw_list->AddText(ImVec2(x + 2, p.y + whiteH - 18), IM_COL32(0, 0, 0, 255), buf);
        }
        wkIdx++;
    }

    // Pass 2: black keys
    wkIdx = 0;
    for (int n = kMinNote; n <= kMaxNote; n++) {
        if (!g_isBlackNote[n % 12]) { wkIdx++; continue; }
        float x = p.x + centerOffset + (wkIdx - 1) * whiteW + whiteW - blackW * 0.5f;
        ImU32 fillCol = blendKey(n, true);
        draw_list->AddRectFilled(ImVec2(x, p.y), ImVec2(x + blackW, p.y + blackH), fillCol);
        draw_list->AddRect(ImVec2(x, p.y), ImVec2(x + blackW, p.y + blackH), IM_COL32(0, 0, 0, 255));
    }

    ImGui::EndChild();

    // ============================================================
    // FM voice parameters (furnace YM2413 OPLL style)
    // ============================================================
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Extract params
    int fb  = g_fmPatch.feedback_lr & 0x07;
    bool l_on = (g_fmPatch.feedback_lr >> 5) & 1;
    bool r_on = (g_fmPatch.feedback_lr >> 6) & 1;
    int m_ar = (g_fmPatch.m_ardr >> 4) & 0x0F;
    int m_dr = g_fmPatch.m_ardr & 0x0F;
    int m_sl = (g_fmPatch.m_slrr >> 4) & 0x0F;
    int m_rr = g_fmPatch.m_slrr & 0x0F;
    int m_tl = g_fmPatch.m_tl & 0x7F;
    int m_ml = g_fmPatch.m_mlws & 0x0F;
    int c_ar = (g_fmPatch.c_ardr >> 4) & 0x0F;
    int c_dr = (g_fmPatch.c_ardr & 0x0F);
    int c_sl = (g_fmPatch.c_slrr >> 4) & 0x0F;
    int c_rr = g_fmPatch.c_slrr & 0x0F;
    int c_tl = g_fmPatch.c_tl & 0x7F;
    int c_ml = g_fmPatch.c_mlws & 0x0F;
    int m_ws = (g_fmPatch.m_mlws >> 5) & 1;
    int c_ws = (g_fmPatch.c_mlws >> 5) & 1;

    bool changed_fm = false;

    // --- Top table: FB, L/R, Algorithm info (furnace fmDetails) ---
    if (ImGui::BeginTable("fmDetails", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::SliderInt("FB", &fb, 0, 7)) changed_fm = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("L", &l_on)) changed_fm = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("R", &r_on)) changed_fm = true;
        ImGui::TableNextColumn();
        ImGui::TextDisabled("YM7129 2-OP FM");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("MOD->CAR");
        ImGui::EndTable();
    }

    ImGui::Spacing();

    // --- Operator table (furnace FMOperators) ---
    // Columns: OP | AR | DR | SL | RR | sep | TL | ML | WS | sep | Wave | Env
    int numCols = 12;
    int numOps = 2;
    float sliderH = 80.0f;
    ImVec2 sliderSz(20.0f, sliderH);

    // Helper: center slider in column (furnace CENTER_VSLIDER)
    auto centerVSlider = [&]() {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.5f - sliderSz.x * 0.5f);
    };

    // Helper: draw waveform preview inside a BeginChild
    auto drawWaveformChild = [&](const char* id, bool ws, ImU32 color) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 childSz(avail.x, sliderH);
        ImGui::BeginChild(id, childSz, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 wsz = ImGui::GetWindowSize();
        ImRect rect(wp, ImVec2(wp.x + wsz.x, wp.y + wsz.y));
        dl->AddRectFilled(rect.Min, rect.Max, IM_COL32(20, 20, 30, 255));
        int steps = (int)wsz.x;
        if (steps >= 2) {
            float midY = wp.y + wsz.y * 0.5f, amp = wsz.y * 0.4f;
            for (int i = 0; i < steps; i++) {
                float t = (float)i / steps * 3.14159265f * 2.0f;
                float v1 = ws ? fabsf(sinf(t)) : sinf(t);
                float t2 = (float)(i + 1) / steps * 3.14159265f * 2.0f;
                float v2 = ws ? fabsf(sinf(t2)) : sinf(t2);
                dl->AddLine(ImVec2(wp.x + i, midY - v1 * amp), ImVec2(wp.x + i + 1, midY - v2 * amp), color);
            }
        }
        ImGui::EndChild();
    };

    // Helper: draw ADSR envelope preview inside a BeginChild (furnace drawFMEnv)
    auto drawADSRChild = [&](const char* id, int ar, int dr, int sl, int rr, int tl_val, int maxTl, ImU32 color) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 childSz(avail.x, sliderH);
        ImGui::BeginChild(id, childSz, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 wsz = ImGui::GetWindowSize();
        ImRect rect(wp, ImVec2(wp.x + wsz.x, wp.y + wsz.y));
        dl->AddRectFilled(rect.Min, rect.Max, IM_COL32(20, 20, 30, 255));

        if (ar > 0) {
            // Furnace drawFMEnv exact formulas (maxArDr=15, maxRr=15, YM7129 has no D2R)
            float arPos = (float)(15 - ar) / 15.0f;
            float drPos = arPos + ((sl / 15.0f) * (float)(15 - dr) / 15.0f);
            float rrPos = (float)(15 - rr) / 15.0f;
            arPos /= 2.0f;
            drPos /= 2.0f;

            float tlN = (maxTl > 0) ? (float)(maxTl - tl_val) / (float)maxTl : 1.0f;
            float slY = tlN + (sl / 15.0f) - (tlN * (sl / 15.0f));

            ImVec2 pos1 = ImLerp(rect.Min, rect.Max, ImVec2(0.0f, 1.0f));
            ImVec2 pos2 = ImLerp(rect.Min, rect.Max, ImVec2(arPos, tlN));
            ImVec2 pos3 = ImLerp(rect.Min, rect.Max, ImVec2(drPos, slY));
            ImVec2 pos4 = ImLerp(rect.Min, rect.Max, ImVec2(drPos, 1.0f));
            ImVec2 posREnd = ImLerp(rect.Min, rect.Max, ImVec2(rrPos, 1.0f));
            ImVec2 posRStart = ImLerp(rect.Min, rect.Max, ImVec2(0.0f, tlN));
            ImVec2 posSLineHEnd = ImLerp(rect.Min, rect.Max, ImVec2(1.0f, slY));
            ImVec2 posSLineVEnd = ImLerp(rect.Min, rect.Max, ImVec2(drPos, 1.0f));

            dl->AddTriangleFilled(posRStart, posREnd, pos1, IM_COL32(40, 40, 50, 128));
            dl->AddLine(ImLerp(rect.Min, rect.Max, ImVec2(drPos, slY)), posSLineHEnd, IM_COL32(60, 60, 60, 128));
            dl->AddLine(pos3, posSLineVEnd, IM_COL32(60, 60, 60, 128));
            dl->AddLine(pos1, pos2, color, 1.5f);
            dl->AddLine(pos2, pos3, color, 1.5f);
            dl->AddLine(pos3, pos4, color, 1.5f);
        }
        ImGui::EndChild();
    };

    if (ImGui::BeginTable("FMOperators", numCols, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Borders | ImGuiTableFlags_BordersV)) {
        // Column setup (furnace: WidthFixed for name/sep, WidthStretch for sliders)
        ImGui::TableSetupColumn("OP",   ImGuiTableColumnFlags_WidthFixed);              // 0
        ImGui::TableSetupColumn("AR",   ImGuiTableColumnFlags_WidthStretch, 0.05f);     // 1
        ImGui::TableSetupColumn("DR",   ImGuiTableColumnFlags_WidthStretch, 0.05f);     // 2
        ImGui::TableSetupColumn("SL",   ImGuiTableColumnFlags_WidthStretch, 0.05f);     // 3
        ImGui::TableSetupColumn("RR",   ImGuiTableColumnFlags_WidthStretch, 0.05f);     // 4
        ImGui::TableSetupColumn("s1",   ImGuiTableColumnFlags_WidthFixed);              // 5
        ImGui::TableSetupColumn("TL",   ImGuiTableColumnFlags_WidthStretch, 0.05f);     // 6
        ImGui::TableSetupColumn("ML",   ImGuiTableColumnFlags_WidthStretch, 0.05f);     // 7
        ImGui::TableSetupColumn("WS",   ImGuiTableColumnFlags_WidthStretch, 0.05f);     // 8
        ImGui::TableSetupColumn("s2",   ImGuiTableColumnFlags_WidthFixed);              // 9
        ImGui::TableSetupColumn("Wave", ImGuiTableColumnFlags_WidthStretch, 0.15f);    // 10
        ImGui::TableSetupColumn("Env",  ImGuiTableColumnFlags_WidthStretch, 0.15f);    // 11

        // Header row — center text in slider columns to align with sliders below
        auto centerText = [&]() {
            ImVec2 textSize = ImGui::CalcTextSize("XX");
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.5f - textSize.x * 0.5f);
        };

        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        ImGui::TableNextColumn(); ImGui::Text("OP");
        ImGui::TableNextColumn(); centerText(); ImGui::Text("AR");
        ImGui::TableNextColumn(); centerText(); ImGui::Text("DR");
        ImGui::TableNextColumn(); centerText(); ImGui::Text("SL");
        ImGui::TableNextColumn(); centerText(); ImGui::Text("RR");
        ImGui::TableNextColumn(); // sep
        ImGui::TableNextColumn(); centerText(); ImGui::Text("TL");
        ImGui::TableNextColumn(); centerText(); ImGui::Text("ML");
        ImGui::TableNextColumn(); centerText(); ImGui::Text("WS");
        ImGui::TableNextColumn(); // sep
        ImGui::TableNextColumn(); centerText(); ImGui::Text("Wave");
        ImGui::TableNextColumn(); centerText(); ImGui::Text("Env");

        // Data: 2 operator rows
        struct OpData { const char* name; int* ar; int* dr; int* sl; int* rr; int* tl; int* ml; int* ws; ImU32 color; };
        OpData ops[2] = {
            {"MOD", &m_ar, &m_dr, &m_sl, &m_rr, &m_tl, &m_ml, &m_ws, IM_COL32(100,255,100,255)},
            {"CAR", &c_ar, &c_dr, &c_sl, &c_rr, &c_tl, &c_ml, &c_ws, IM_COL32(100,150,255,255)},
        };

        for (int i = 0; i < numOps; i++) {
            OpData& o = ops[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", o.name);

            // AR
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10);
            if (ImGui::VSliderInt("##AR", sliderSz, o.ar, 0, 15)) changed_fm = true;
            ImGui::PopID();

            // DR
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 1);
            if (ImGui::VSliderInt("##DR", sliderSz, o.dr, 0, 15)) changed_fm = true;
            ImGui::PopID();

            // SL
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 2);
            if (ImGui::VSliderInt("##SL", sliderSz, o.sl, 0, 15)) changed_fm = true;
            ImGui::PopID();

            // RR
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 3);
            if (ImGui::VSliderInt("##RR", sliderSz, o.rr, 0, 15)) changed_fm = true;
            ImGui::PopID();

            // sep
            ImGui::TableNextColumn();

            // TL
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 4);
            if (ImGui::VSliderInt("##TL", sliderSz, o.tl, 0, 127)) changed_fm = true;
            ImGui::PopID();

            // ML
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 5);
            if (ImGui::VSliderInt("##ML", sliderSz, o.ml, 0, 15)) changed_fm = true;
            ImGui::PopID();

            // WS
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 6);
            if (ImGui::VSliderInt("##WS", sliderSz, o.ws, 0, 1)) changed_fm = true;
            ImGui::PopID();

            // sep
            ImGui::TableNextColumn();

            // sep
            ImGui::TableNextColumn();

            // Wave preview (each op row has its own)
            ImGui::TableNextColumn();
            ImGui::PushID(i * 100 + 0);
            drawWaveformChild("##wav", *o.ws != 0, o.color);
            ImGui::PopID();

            // ADSR preview (each op row has its own)
            ImGui::TableNextColumn();
            ImGui::PushID(i * 100 + 1);
            drawADSRChild("##env", *o.ar, *o.dr, *o.sl, *o.rr, *o.tl, 127, o.color);
            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    if (changed_fm) {
        g_fmPatch.feedback_lr = (fb & 0x07) | (l_on ? (1<<5) : 0) | (r_on ? (1<<6) : 0);
        g_fmPatch.m_ardr = ((m_ar & 0x0F) << 4) | (m_dr & 0x0F);
        g_fmPatch.m_slrr = ((m_sl & 0x0F) << 4) | (m_rr & 0x0F);
        g_fmPatch.m_tl   = m_tl & 0x7F;
        g_fmPatch.m_mlws = (m_ml & 0x0F) | ((m_ws & 1) << 5);
        g_fmPatch.c_ardr = ((c_ar & 0x0F) << 4) | (c_dr & 0x0F);
        g_fmPatch.c_slrr = ((c_sl & 0x0F) << 4) | (c_rr & 0x0F);
        g_fmPatch.c_tl   = c_tl & 0x7F;
        g_fmPatch.c_mlws = (c_ml & 0x0F) | ((c_ws & 1) << 5);
        reload_fm_patch();
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
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // ViewportsEnable 暂时关闭 — 窗口不能拖出成为独立窗口
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
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

        // Decay hit animations + piano key levels
        float dt = io.DeltaTime;
        for (int i = 0; i < 12; i++) {
            if (g_hitAnim[i] > 0.0f) {
                g_hitAnim[i] -= dt * 6.0f;
                if (g_hitAnim[i] < 0.0f) g_hitAnim[i] = 0.0f;
            }
        }
        for (int i = 0; i < 128; i++) {
            if (g_pianoLevel[i] > 0.0f) {
                // Decay only after key release (key down keeps level at 1.0 via WM_KEYDOWN repeat guard)
                // Decay speed: ~0.3s to fade out (same feel as FM release)
                g_pianoLevel[i] -= dt * 3.0f;
                if (g_pianoLevel[i] < 0.0f) g_pianoLevel[i] = 0.0f;
                if (g_pianoLevel[i] <= 0.0f && g_pianoChPlaying[i] >= 0) {
                    g_pianoChPlaying[i] = -1;
                }
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
        RenderPiano();

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

/**
 * YM7129 Drum Pad - ImGui + DX11
 *
 * 12 drum pads (6 types x L/H), mouse + keyboard.
 * Connect/Disconnect/Reset. Delay slider 0-200us.
 * Uses verified 3-byte raw protocol: [slot, 0x80, addr] [slot, 0x81, data]
 */

// Include winsock2.h before windows.h (required by imgui_impl_win32.h)
#include <winsock2.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <tchar.h>

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
static int   g_pianoNotePlaying = -1;  // current MIDI note, -1=none
static int   g_pianoChannel = 0;       // always use channel 0 (single channel)
static float g_pianoLevel = 0.0f;
static int   g_velocity = 100;        // global velocity (0-127)
static int   g_testE = 0;             // $00 TEST-E register

// Editable FM patch (start with simple sine-like tone)
static ym7129_patch_t g_fmPatch = {
    0x60, // feedback_lr: L+R, FB=0
    0x00, // m_mlws: DM=0, EG-TYP=0, ML=0
    0xFF, // m_tl: mute (8bit)
    0x00, // m_ardr
    0x00, // m_slrr
    0x0D, // c_mlws: AM=0, EG-TYP=0, WS=0, ML=13
    0x30, // c_tl: volume (7bit)
    0x58, // c_ardr: AR=5, DR=8
    0x0B, // c_slrr: SL=0, RR=11
};

// ============================================================
// Preset patches (from wuqi hardware capture)
// ============================================================
struct PresetEntry { const char* name; ym7129_patch_t patch; };
static const PresetEntry g_presets[] = {
    {"Sine", {
        0xE0, // feedback_lr: L+R, bit7=1, FB=0
        0x00, // m_mlws: DM=0, ML=0
        0xFF, // m_tl: mute
        0x00, // m_ardr
        0x00, // m_slrr
        0x11, // c_mlws: EG-TYP=1, ML=1
        0x30, // c_tl
        0x00, // c_ardr
        0x05, // c_slrr: SL=0, RR=5
    }},
    {"Carrier", {
        0xE0, // feedback_lr: L+R, bit7=1, FB=0
        0x00, // m_mlws: DM=0, ML=0
        0x94, // m_tl: weak modulation
        0x00, // m_ardr
        0x00, // m_slrr
        0x01, // c_mlws: ML=1
        0x39, // c_tl
        0x00, // c_ardr
        0x06, // c_slrr: SL=0, RR=6
    }},
    {"UFO", {
        0xC7, // feedback_lr: R=1, bit7=1, FB=7
        0x90, // m_mlws: EG-TYP=1, DM=0, ML=0
        0x51, // m_tl
        0x90, // m_ardr: AR=9, DR=0
        0x00, // m_slrr
        0x90, // c_mlws: EG-TYP=1, ML=0
        0x4A, // c_tl
        0x90, // c_ardr: AR=9, DR=0
        0x00, // c_slrr
    }},
    {"Chord", {
        0xE5, // feedback_lr: L+R, bit7=1, FB=5
        0x2A, // m_mlws: DM=1, ML=10
        0x94, // m_tl: weak modulation
        0xF6, // m_ardr: AR=15, DR=6
        0x32, // m_slrr: SL=3, RR=2
        0x04, // c_mlws: ML=4
        0x42, // c_tl
        0xB5, // c_ardr: AR=11, DR=5
        0x00, // c_slrr: SL=0, RR=0
    }},
};
static const int g_numPresets = sizeof(g_presets) / sizeof(PresetEntry);

// User preset slot (saved to INI)
static ym7129_patch_t g_userPatch = {0};

static void reload_fm_patch() {
    if (!g_connected) return;
    for (int ch = 0; ch < 8; ch++)
        ym7129_load_patch(g_slot, ch, &g_fmPatch);
}

static char g_configPath[MAX_PATH] = "";

static void load_fm_patch_config() {
    if (g_configPath[0] == '\0') return;
    g_fmPatch.feedback_lr = GetPrivateProfileIntA("FMPatch", "feedback_lr", g_fmPatch.feedback_lr, g_configPath);
    g_fmPatch.m_mlws       = GetPrivateProfileIntA("FMPatch", "m_mlws", g_fmPatch.m_mlws, g_configPath);
    g_fmPatch.m_tl         = GetPrivateProfileIntA("FMPatch", "m_tl", g_fmPatch.m_tl, g_configPath);
    g_fmPatch.m_ardr       = GetPrivateProfileIntA("FMPatch", "m_ardr", g_fmPatch.m_ardr, g_configPath);
    g_fmPatch.m_slrr       = GetPrivateProfileIntA("FMPatch", "m_slrr", g_fmPatch.m_slrr, g_configPath);
    g_fmPatch.c_mlws       = GetPrivateProfileIntA("FMPatch", "c_mlws", g_fmPatch.c_mlws, g_configPath);
    g_fmPatch.c_tl         = GetPrivateProfileIntA("FMPatch", "c_tl", g_fmPatch.c_tl, g_configPath);
    g_fmPatch.c_ardr       = GetPrivateProfileIntA("FMPatch", "c_ardr", g_fmPatch.c_ardr, g_configPath);
    g_fmPatch.c_slrr       = GetPrivateProfileIntA("FMPatch", "c_slrr", g_fmPatch.c_slrr, g_configPath);
}

static void save_fm_patch_config() {
    if (g_configPath[0] == '\0') return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", g_fmPatch.feedback_lr); WritePrivateProfileStringA("FMPatch", "feedback_lr", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_fmPatch.m_mlws);       WritePrivateProfileStringA("FMPatch", "m_mlws", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_fmPatch.m_tl);         WritePrivateProfileStringA("FMPatch", "m_tl", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_fmPatch.m_ardr);       WritePrivateProfileStringA("FMPatch", "m_ardr", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_fmPatch.m_slrr);       WritePrivateProfileStringA("FMPatch", "m_slrr", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_fmPatch.c_mlws);       WritePrivateProfileStringA("FMPatch", "c_mlws", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_fmPatch.c_tl);         WritePrivateProfileStringA("FMPatch", "c_tl", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_fmPatch.c_ardr);       WritePrivateProfileStringA("FMPatch", "c_ardr", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_fmPatch.c_slrr);       WritePrivateProfileStringA("FMPatch", "c_slrr", buf, g_configPath);
}

// ============================================================
// User preset persistence (INI [UserPreset] section)
// ============================================================
static void save_user_preset_config() {
    if (g_configPath[0] == '\0') return;
    g_userPatch = g_fmPatch;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", g_userPatch.feedback_lr); WritePrivateProfileStringA("UserPreset", "feedback_lr", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_userPatch.m_mlws);       WritePrivateProfileStringA("UserPreset", "m_mlws", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_userPatch.m_tl);         WritePrivateProfileStringA("UserPreset", "m_tl", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_userPatch.m_ardr);       WritePrivateProfileStringA("UserPreset", "m_ardr", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_userPatch.m_slrr);       WritePrivateProfileStringA("UserPreset", "m_slrr", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_userPatch.c_mlws);       WritePrivateProfileStringA("UserPreset", "c_mlws", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_userPatch.c_tl);         WritePrivateProfileStringA("UserPreset", "c_tl", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_userPatch.c_ardr);       WritePrivateProfileStringA("UserPreset", "c_ardr", buf, g_configPath);
    snprintf(buf, sizeof(buf), "%d", g_userPatch.c_slrr);       WritePrivateProfileStringA("UserPreset", "c_slrr", buf, g_configPath);
}

static void load_user_preset_config() {
    if (g_configPath[0] == '\0') return;
    g_userPatch.feedback_lr = GetPrivateProfileIntA("UserPreset", "feedback_lr", g_userPatch.feedback_lr, g_configPath);
    g_userPatch.m_mlws       = GetPrivateProfileIntA("UserPreset", "m_mlws", g_userPatch.m_mlws, g_configPath);
    g_userPatch.m_tl         = GetPrivateProfileIntA("UserPreset", "m_tl", g_userPatch.m_tl, g_configPath);
    g_userPatch.m_ardr       = GetPrivateProfileIntA("UserPreset", "m_ardr", g_userPatch.m_ardr, g_configPath);
    g_userPatch.m_slrr       = GetPrivateProfileIntA("UserPreset", "m_slrr", g_userPatch.m_slrr, g_configPath);
    g_userPatch.c_mlws       = GetPrivateProfileIntA("UserPreset", "c_mlws", g_userPatch.c_mlws, g_configPath);
    g_userPatch.c_tl         = GetPrivateProfileIntA("UserPreset", "c_tl", g_userPatch.c_tl, g_configPath);
    g_userPatch.c_ardr       = GetPrivateProfileIntA("UserPreset", "c_ardr", g_userPatch.c_ardr, g_configPath);
    g_userPatch.c_slrr       = GetPrivateProfileIntA("UserPreset", "c_slrr", g_userPatch.c_slrr, g_configPath);
}

static void piano_key_down(int note, int octave) {
    if (!g_connected) return;
    int midi_note = (octave + 2) * 12 + note;

    // Single channel: kill previous note first if any
    if (g_pianoNotePlaying >= 0) {
        ym7129_note_kill(g_slot, (uint8_t)g_pianoChannel);
    }

    ym7129_note_on(g_slot, (uint8_t)g_pianoChannel, (uint8_t)midi_note, (uint8_t)g_velocity);
    g_pianoNotePlaying = midi_note;
    g_pianoLevel = 1.0f;
    log_cmd("Piano ON: midi=%d", midi_note);
}

static void piano_key_up(int note, int octave) {
    if (!g_connected) return;
    int midi_note = (octave + 2) * 12 + note;
    if (g_pianoNotePlaying == midi_note) {
        // Release envelope (RR-based decay)
        ym7129_note_off(g_slot, (uint8_t)g_pianoChannel);
        g_pianoNotePlaying = -1;
        log_cmd("Piano OFF: midi=%d", midi_note);
    }
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
    // Restore all FM L/R to L+R before drum (YM7129 drum output routes through $70-$77)
    for (int ch = 0; ch < 8; ch++)
        ym7129_write_reg(g_slot, 0x70 + ch, 0x60);
    ym7129_drum_mute(g_slot);
    log_cmd("Strike: %s (Rhy=%d L/H=%d Vol=%d)", p->name, p->rhy_type, p->lh, g_volume);
    ym7129_drum_strike(g_slot, p->channel, p->rhy_type, p->lh, (uint8_t)g_volume);
    // Reload FM patch L/R/FB for the piano channel
    ym7129_write_reg(g_slot, 0x70 + g_pianoChannel, g_fmPatch.feedback_lr);
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
    ym7129_init(g_slot);
    Sleep(100);
    log_cmd("Connected");
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

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
    ImGui::Text("Vol: %d", g_volume);
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

    // Drum register debug: $70-$88 (FM L/R + unknown + drum control)
    static bool drum_pan_debug = false;
    static uint8_t drum_pan[25] = {}; // $70-$88 = 25 regs
    static bool drum_pan_inited = false;
    if (!drum_pan_inited) {
        for (int i = 0; i < 8; i++) drum_pan[i] = 0x60; // $70-$77 FM L+R
        drum_pan[20] = 0x00; // $84 trigger = 0
        drum_pan_inited = true;
    }
    ImGui::Spacing();
    ImGui::Checkbox("Reg Debug ($70-$88)", &drum_pan_debug);
    if (drum_pan_debug) {
        if (ImGui::BeginTable("RegDebug", 4, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            ImGui::TableNextColumn(); ImGui::Text("Reg");
            ImGui::TableNextColumn(); ImGui::Text("Guess");
            ImGui::TableNextColumn(); ImGui::Text("Value");
            ImGui::TableNextColumn(); ImGui::Text("Bits");
            for (int i = 0; i < 25; i++) {
                int reg = 0x70 + i;
                const char* guess;
                if (i < 8)       guess = "FM L/R/FB";
                else if (i < 16) guess = "Drum Pan?";
                else if (i < 20) guess = "Drum Ctrl";
                else             guess = "Drum Trg";
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("$%02X", reg);
                ImGui::TableNextColumn(); ImGui::Text("%s", guess);
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(50);
                int pv = drum_pan[i];
                if (ImGui::InputInt("##reg", &pv, 0)) drum_pan[i] = (uint8_t)(pv & 0xFF);
                ImGui::PopID();
                ImGui::TableNextColumn();
                if (i < 8)       ImGui::TextDisabled("R L ?? FB");
                else if (i < 16) ImGui::TextDisabled("unknown");
                else if (i < 20) ImGui::TextDisabled("L/H Rhy Vol");
                else             ImGui::TextDisabled("k3 k2 k1 k0");
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ImGui::Button("Apply All")) {
            for (int i = 0; i < 25; i++)
                ym7129_write_reg(g_slot, 0x70 + i, drum_pan[i]);
            log_cmd("Reg: $70-$88 applied");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
            for (int i = 0; i < 8; i++) drum_pan[i] = 0x60;
            for (int i = 8; i < 25; i++) drum_pan[i] = 0x00;
        }
    }

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
        if (midiNote < 0 || midiNote >= 128 || midiNote != g_pianoNotePlaying || g_pianoLevel <= 0.0f) {
            return isBlack ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
        }
        float lv = powf(g_pianoLevel, 0.5f);
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
    int fb_bit7 = (g_fmPatch.feedback_lr >> 7) & 1;  // unknown global bit
    bool bit7_on = fb_bit7 != 0;    int m_ar = (g_fmPatch.m_ardr >> 4) & 0x0F;
    int m_dr = g_fmPatch.m_ardr & 0x0F;
    int m_sl = (g_fmPatch.m_slrr >> 4) & 0x0F;
    int m_rr = g_fmPatch.m_slrr & 0x0F;
    int m_tl = g_fmPatch.m_tl;  // 8bit (wuqi doc: modulator TL 0-255)
    int m_ml = g_fmPatch.m_mlws & 0x0F;
    int m_eg = (g_fmPatch.m_mlws >> 4) & 1;  // modulator EG-TYP (bit4)
    int m_vib = (g_fmPatch.m_mlws >> 6) & 1; // modulator VIB (bit6)
    int m_dm = (g_fmPatch.m_mlws >> 5) & 1;  // modulator DM (bit5)
    int c_ar = (g_fmPatch.c_ardr >> 4) & 0x0F;
    int c_dr = (g_fmPatch.c_ardr & 0x0F);
    int c_sl = (g_fmPatch.c_slrr >> 4) & 0x0F;
    int c_rr = g_fmPatch.c_slrr & 0x0F;
    int c_tl = g_fmPatch.c_tl & 0x7F;
    int c_dp = (g_fmPatch.c_tl >> 7) & 1;    // carrier DP (bit7)
    int c_ml = g_fmPatch.c_mlws & 0x0F;
    int c_eg = (g_fmPatch.c_mlws >> 4) & 1;  // carrier EG-TYP (bit4)
    int c_ws = (g_fmPatch.c_mlws >> 5) & 1;  // carrier WS (bit5)
    int c_am = (g_fmPatch.c_mlws >> 6) & 1;  // carrier AM (bit6)

    bool changed_fm = false;

    // --- Top row: FB, L/R, bit7, Velocity, TEST-E (global params) ---
    if (ImGui::SliderInt("FB", &fb, 0, 7)) changed_fm = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("L", &l_on)) changed_fm = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("R", &r_on)) changed_fm = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("PM/FM", &bit7_on)) { fb_bit7 = bit7_on ? 1 : 0; changed_fm = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("bit7=0: FM synthesis (true FM)\nbit7=1: PM synthesis (YM2413-like)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::SliderInt("Vol", &g_velocity, 0, 127)) {}
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    if (ImGui::InputInt("TEST-E", &g_testE, 0)) {
        g_testE &= 0x0F; // 4 bits only
        if (g_connected) ym7129_write_reg(g_slot, 0x00, (uint8_t)g_testE);
    }

    ImGui::Spacing();

    // --- Operator table (furnace FMOperators) ---
    // Columns: OP | AR | DR | SL | RR | sep | TL | ML | EG | VIB/WS | DM/AM | DP | sep | Wave | Env
    int numCols = 16;
    int numOps = 2;
    float sliderH = 65.0f;
    float sliderW = 35.0f;  // wider to fit 3-digit numbers (TL: 0-255)
    ImVec2 sliderSz(sliderW, sliderH);

    // Helper: center slider in column (furnace CENTER_VSLIDER)
    auto centerVSlider = [&]() {
        float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.5f - sliderSz.x * 0.5f;
        ImGui::SetCursorPosX(x);
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::SetCursorPosX(x);
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
        ImGui::TableSetupColumn("OP",   ImGuiTableColumnFlags_WidthFixed);              // 0
        ImGui::TableSetupColumn("AR",   ImGuiTableColumnFlags_WidthStretch, 0.04f);     // 1
        ImGui::TableSetupColumn("DR",   ImGuiTableColumnFlags_WidthStretch, 0.04f);     // 2
        ImGui::TableSetupColumn("SL",   ImGuiTableColumnFlags_WidthStretch, 0.04f);     // 3
        ImGui::TableSetupColumn("RR",   ImGuiTableColumnFlags_WidthStretch, 0.04f);     // 4
        ImGui::TableSetupColumn("s1",   ImGuiTableColumnFlags_WidthFixed);              // 5
        ImGui::TableSetupColumn("TL",   ImGuiTableColumnFlags_WidthStretch, 0.04f);     // 6
        ImGui::TableSetupColumn("ML",   ImGuiTableColumnFlags_WidthStretch, 0.04f);     // 7
        ImGui::TableSetupColumn("EG",   ImGuiTableColumnFlags_WidthStretch, 0.03f);     // 8
        ImGui::TableSetupColumn("DM/WS",ImGuiTableColumnFlags_WidthStretch, 0.03f);     // 9
        ImGui::TableSetupColumn("VIB/AM",ImGuiTableColumnFlags_WidthStretch, 0.03f);    // 10
        ImGui::TableSetupColumn("DP",   ImGuiTableColumnFlags_WidthStretch, 0.03f);     // 11
        ImGui::TableSetupColumn("s2",   ImGuiTableColumnFlags_WidthFixed);              // 12
        ImGui::TableSetupColumn("Wave", ImGuiTableColumnFlags_WidthStretch, 0.12f);    // 13
        ImGui::TableSetupColumn("Env",  ImGuiTableColumnFlags_WidthStretch, 0.12f);    // 14

        // Header row — center text in slider columns to align with sliders below
        auto centerText = [&]() {
            ImVec2 textSize = ImGui::CalcTextSize("XX");
            float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.5f - textSize.x * 0.5f;
            ImGui::SetCursorPosX(x);
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::SetCursorPosX(x);
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
        ImGui::TableNextColumn(); centerText(); ImGui::Text("EG");
        ImGui::TableNextColumn(); centerText(); ImGui::Text("DM/WS");
        ImGui::TableNextColumn(); centerText(); ImGui::Text("VIB/AM");
        ImGui::TableNextColumn(); centerText(); ImGui::Text("DP");
        ImGui::TableNextColumn(); // sep
        ImGui::TableNextColumn(); centerText(); ImGui::Text("Wave");
        ImGui::TableNextColumn(); centerText(); ImGui::Text("Env");

        // Data: 2 operator rows
        // dp: carrier only (modulator always NULL)
        struct OpData { const char* name; int* ar; int* dr; int* sl; int* rr; int* tl; int tlMax; int* ml; int* eg; int* dm_ws; int* vib_am; int* dp; ImU32 color; };
        OpData ops[2] = {
            {"MOD", &m_ar, &m_dr, &m_sl, &m_rr, &m_tl, 255, &m_ml, &m_eg, &m_dm, &m_vib, NULL, IM_COL32(100,255,100,255)},
            {"CAR", &c_ar, &c_dr, &c_sl, &c_rr, &c_tl, 127, &c_ml, &c_eg, &c_ws, &c_am, &c_dp, IM_COL32(100,150,255,255)},
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

            // TL (MOD: 0-255, CAR: 0-127)
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 4);
            if (ImGui::VSliderInt("##TL", sliderSz, o.tl, 0, o.tlMax)) changed_fm = true;
            ImGui::PopID();

            // ML
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 5);
            if (ImGui::VSliderInt("##ML", sliderSz, o.ml, 0, 15)) changed_fm = true;
            ImGui::PopID();

            // EG-TYP (bit4)
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 6);
            if (ImGui::VSliderInt("##EG", sliderSz, o.eg, 0, 1)) changed_fm = true;
            ImGui::PopID();

            // DM (MOD) or WS (CAR)
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 7);
            if (o.dm_ws) {
                if (ImGui::VSliderInt("##DMWS", sliderSz, o.dm_ws, 0, 1)) changed_fm = true;
            }
            ImGui::PopID();

            // VIB (MOD bit6) or AM (CAR bit6)
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 8);
            if (o.vib_am) {
                if (ImGui::VSliderInt("##VIBAM", sliderSz, o.vib_am, 0, 1)) changed_fm = true;
            }
            ImGui::PopID();

            // DP (carrier TL bit7, modulator has none)
            ImGui::TableNextColumn(); centerVSlider();
            ImGui::PushID(i * 10 + 9);
            if (o.dp) {
                if (ImGui::VSliderInt("##DP", sliderSz, o.dp, 0, 1)) changed_fm = true;
            }
            ImGui::PopID();

            // sep
            ImGui::TableNextColumn();

            // Wave preview (each op row has its own)
            ImGui::TableNextColumn();
            ImGui::PushID(i * 100 + 0);
            bool waveWs = o.dm_ws ? (*o.dm_ws != 0) : false;
            drawWaveformChild("##wav", waveWs, o.color);
            ImGui::PopID();

            // ADSR preview (each op row has its own)
            ImGui::TableNextColumn();
            ImGui::PushID(i * 100 + 1);
            drawADSRChild("##env", *o.ar, *o.dr, *o.sl, *o.rr, *o.tl, o.tlMax, o.color);
            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    if (changed_fm) {
        g_fmPatch.feedback_lr = (fb & 0x07) | (l_on ? (1<<5) : 0) | (r_on ? (1<<6) : 0) | (fb_bit7 ? (1<<7) : 0);
        g_fmPatch.m_ardr = ((m_ar & 0x0F) << 4) | (m_dr & 0x0F);
        g_fmPatch.m_slrr = ((m_sl & 0x0F) << 4) | (m_rr & 0x0F);
        g_fmPatch.m_tl   = m_tl & 0xFF;                                                    // 8bit modulator TL
        g_fmPatch.m_mlws = (m_ml & 0x0F) | ((m_eg & 1) << 4) | ((m_dm & 1) << 5) | ((m_vib & 1) << 6);  // bit4=EG, bit5=DM, bit6=VIB
        g_fmPatch.c_ardr = ((c_ar & 0x0F) << 4) | (c_dr & 0x0F);
        g_fmPatch.c_slrr = ((c_sl & 0x0F) << 4) | (c_rr & 0x0F);
        g_fmPatch.c_tl   = (c_tl & 0x7F) | ((c_dp & 1) << 7);                              // bit7=DP, bit6-0=vol
        g_fmPatch.c_mlws = (c_ml & 0x0F) | ((c_eg & 1) << 4) | ((c_ws & 1) << 5) | ((c_am & 1) << 6);   // bit4=EG, bit5=WS, bit6=AM
        reload_fm_patch();
        save_fm_patch_config();
        save_user_preset_config();
    }

    // ============================================================
    // Preset buttons (wuqi hardware captured patches)
    // ============================================================
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Presets:");

    // Factory presets: Sine, Carrier, UFO, Chord
    for (int i = 0; i < g_numPresets; i++) {
        ImGui::PushID(i);
        ImGui::SameLine();
        if (ImGui::SmallButton(g_presets[i].name)) {
            g_fmPatch = g_presets[i].patch;
            reload_fm_patch();
            save_fm_patch_config();
            save_user_preset_config();
            log_cmd("Preset: %s", g_presets[i].name);
        }
        ImGui::PopID();
    }

    // User preset: Save + Load
    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();
    ImGui::PushID(100);
    if (ImGui::SmallButton("Save User")) {
        g_userPatch = g_fmPatch;
        save_user_preset_config();
        log_cmd("User preset saved");
    }
    ImGui::PopID();

    ImGui::PushID(101);
    ImGui::SameLine();
    if (ImGui::SmallButton("Load User")) {
        // Check if user preset has ever been saved (non-zero feedback_lr is a good indicator)
        bool hasUserPreset = false;
        if (g_configPath[0] != '\0') {
            char buf[32] = {};
            GetPrivateProfileStringA("UserPreset", "feedback_lr", "", buf, sizeof(buf), g_configPath);
            hasUserPreset = (buf[0] != '\0');
        }
        if (hasUserPreset) {
            g_fmPatch = g_userPatch;
            reload_fm_patch();
            save_fm_patch_config();
            log_cmd("User preset loaded");
        } else {
            log_cmd("No user preset saved yet");
        }
    }
    ImGui::PopID();

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
        // Also set config path for FM patch (same dir, different file)
        if (lastSlash) {
            wcscpy(lastSlash + 1, L"ym7129_config.ini");
            snprintf(g_configPath, sizeof(g_configPath), "%S", wbuf);
        }
    }
    load_fm_patch_config();
    load_user_preset_config();
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
        // Decay piano visual level after key release
        if (g_pianoLevel > 0.0f && g_pianoNotePlaying < 0) {
            g_pianoLevel -= dt * 3.0f;
            if (g_pianoLevel < 0.0f) g_pianoLevel = 0.0f;
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

/**
 * YM7129 Drum Pad - Win32 GDI
 *
 * Pure Win32 GDI drum pad. No dependencies beyond ym7129/spfm drivers.
 * 12 drum pads: 6 types x 2 (L/H), mapped to mouse clicks + numpad keys.
 * Uses verified 3-byte raw protocol: [slot, 0x80, addr] [slot, 0x81, data]
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "spfm.h"
#include "ym7129.h"

// ============================================================
// Drum pad definitions (12 pads: 6 types x 2 L/H)
// ============================================================
typedef struct {
    const char *name;
    uint8_t    rhy_type; // 0=BD,1=SD,2=C.HH,3=O.HH,4=Tom,5=Ride
    uint8_t    lh;       // 0=H, 1=L
    uint8_t    channel;  // $80-$83 channel
} drum_pad_t;

static const drum_pad_t DRUM_PADS[12] = {
    { "BD  H",  0, 0, 0 }, { "BD  L",  0, 1, 0 },
    { "SD  H",  1, 0, 1 }, { "SD  L",  1, 1, 1 },
    { "C.HH H", 2, 0, 2 }, { "C.HH L", 2, 1, 2 },
    { "O.HH H", 3, 0, 3 }, { "O.HH L", 3, 1, 3 },
    { "Tom H",  4, 0, 0 }, { "Tom L",  4, 1, 0 },
    { "Ride H", 5, 0, 1 }, { "Ride L", 5, 1, 1 },
};

// Numpad key mapping: VK_NUMPAD0..9, VK_MULTIPLY, VK_SUBTRACT, VK_ADD
static const UINT PAD_KEYS[12] = {
    VK_NUMPAD7, VK_NUMPAD8,    // BD H/L
    VK_NUMPAD4, VK_NUMPAD5,    // SD H/L
    VK_NUMPAD1, VK_NUMPAD2,    // C.HH H/L
    VK_NUMPAD0, VK_DECIMAL,     // O.HH H/L
    VK_MULTIPLY, VK_SUBTRACT,   // Tom H/L
    VK_DIVIDE,  VK_ADD,        // Ride H/L
};

// ============================================================
// Colors
// ============================================================
#define COL_BG         RGB(30, 30, 40)
#define COL_PAD_NORMAL RGB(60, 60, 80)
#define COL_PAD_HOVER  RGB(80, 80, 110)
#define COL_PAD_HIT    RGB(200, 60, 60)
#define COL_PAD_BORDER RGB(120, 120, 160)
#define COL_TEXT       RGB(220, 220, 240)
#define COL_TEXT_DIM   RGB(140, 140, 160)
#define COL_CONNECTED  RGB(80, 200, 80)
#define COL_DISCONNECTED RGB(200, 80, 80)

// ============================================================
// Layout
// ============================================================
#define PAD_COLS   2
#define PAD_ROWS   6
#define PAD_W      140
#define PAD_H      52
#define PAD_GAP_X  10
#define PAD_GAP_Y  6
#define PAD_AREA_X 20
#define PAD_AREA_Y 40
#define PAD_AREA_W (PAD_COLS * PAD_W + (PAD_COLS - 1) * PAD_GAP_X)
#define PAD_AREA_H (PAD_ROWS * PAD_H + (PAD_ROWS - 1) * PAD_GAP_Y)

#define WIN_W (PAD_AREA_X * 2 + PAD_AREA_W + 20)
#define WIN_H (PAD_AREA_Y + PAD_AREA_H + 60)

// ============================================================
// State
// ============================================================
static HINSTANCE g_hinst;
static HWND      g_hwnd;
static HFONT     g_hFontPad;
static HFONT     g_hFontStatus;
static HFONT     g_hFontKey;

static int      g_connected = 0;
static uint8_t  g_slot = 0;
static int      g_hitPad = -1;      // currently animating pad (-1 = none)
static int      g_hoverPad = -1;    // mouse hover pad
static int      g_volume = 0;       // 0=loud, 15=quiet

// Hit animation timer
static UINT_PTR g_hitTimer = 0;
#define HIT_FLASH_MS 120

// ============================================================
// Helpers
// ============================================================
static RECT pad_rect(int i) {
    int col = i % PAD_COLS;
    int row = i / PAD_COLS;
    RECT r;
    r.left   = PAD_AREA_X + col * (PAD_W + PAD_GAP_X);
    r.top    = PAD_AREA_Y + row * (PAD_H + PAD_GAP_Y);
    r.right  = r.left + PAD_W;
    r.bottom = r.top  + PAD_H;
    return r;
}

static int pad_from_point(int x, int y) {
    for (int i = 0; i < 12; i++) {
        RECT r = pad_rect(i);
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom)
            return i;
    }
    return -1;
}

static void CALLBACK HitTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time) {
    (void)msg; (void)id; (void)time;
    KillTimer(hwnd, g_hitTimer);
    g_hitTimer = 0;
    g_hitPad = -1;
    InvalidateRect(hwnd, NULL, FALSE);
}

static void strike_drum(int pad_idx) {
    if (!g_connected || pad_idx < 0 || pad_idx >= 12) return;
    const drum_pad_t *p = &DRUM_PADS[pad_idx];
    ym7129_drum_strike(g_slot, p->channel, p->rhy_type, p->lh, (uint8_t)g_volume);

    g_hitPad = pad_idx;
    if (g_hitTimer) KillTimer(g_hwnd, g_hitTimer);
    g_hitTimer = SetTimer(g_hwnd, 1, HIT_FLASH_MS, HitTimerProc);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ============================================================
// Drawing
// ============================================================
static void DrawRoundRect(HDC hdc, RECT *r, COLORREF fill, COLORREF border, int radius) {
    HBRUSH hbr = CreateSolidBrush(fill);
    HPEN   hpn = CreatePen(PS_SOLID, 2, border);

    SelectObject(hdc, hbr);
    SelectObject(hdc, hpn);

    RoundRect(hdc, r->left, r->top, r->right, r->bottom, radius, radius);

    DeleteObject(hbr);
    DeleteObject(hpn);
}

static void DrawPad(HDC hdc, int i) {
    RECT r = pad_rect(i);
    int is_hit   = (i == g_hitPad);
    int is_hover = (i == g_hoverPad && !is_hit);

    COLORREF fill = is_hit ? COL_PAD_HIT : (is_hover ? COL_PAD_HOVER : COL_PAD_NORMAL);
    DrawRoundRect(hdc, &r, fill, COL_PAD_BORDER, 8);

    // Pad name (left)
    SetTextColor(hdc, COL_TEXT);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_hFontPad);
    RECT tr = { r.left + 12, r.top + 4, r.right - 50, r.bottom - 4 };
    DrawText(hdc, DRUM_PADS[i].name, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Key hint (right)
    SelectObject(hdc, g_hFontKey);
    SetTextColor(hdc, COL_TEXT_DIM);
    char keyhint[8];
    const char *keynames[] = {
        "Num7","Num8","Num4","Num5","Num1","Num2",
        "Num0",".","*","-","/","+"
    };
    snprintf(keyhint, sizeof(keyhint), "[%s]", keynames[i]);
    RECT kr = { r.right - 52, r.top + 4, r.right - 4, r.bottom - 4 };
    DrawText(hdc, keyhint, -1, &kr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void DrawStatus(HDC hdc, int w, int h) {
    RECT sr = { PAD_AREA_X, h - 30, w - 20, h - 8 };

    const char *status_text;
    COLORREF col;
    if (g_connected) {
        status_text = "Connected - SPFM device active";
        col = COL_CONNECTED;
    } else {
        status_text = "Disconnected";
        col = COL_DISCONNECTED;
    }

    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_hFontStatus);
    DrawText(hdc, status_text, -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Volume indicator
    char vol[32];
    snprintf(vol, sizeof(vol), "Vol: %d", g_volume);
    RECT vr = { sr.right - 60, sr.top, sr.right, sr.bottom };
    DrawText(hdc, vol, -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    // Background
    RECT cr;
    GetClientRect(hwnd, &cr);
    HBRUSH bgBr = CreateSolidBrush(COL_BG);
    FillRect(hdc, &cr, bgBr);
    DeleteObject(bgBr);

    // Title
    SetTextColor(hdc, COL_TEXT);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_hFontStatus);
    RECT tr = { PAD_AREA_X, 10, cr.right - 20, 32 };
    DrawText(hdc, "YM7129 Drum Pad", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Pads
    for (int i = 0; i < 12; i++) {
        DrawPad(hdc, i);
    }

    // Status bar
    DrawStatus(hdc, cr.right, cr.bottom);

    EndPaint(hwnd, &ps);
}

// ============================================================
// Window Procedure
// ============================================================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_PAINT:
        OnPaint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        int mx = LOWORD(l), my = HIWORD(l);
        int pad = pad_from_point(mx, my);
        if (pad != g_hoverPad) {
            g_hoverPad = pad;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hoverPad = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = LOWORD(l), my = HIWORD(l);
        int pad = pad_from_point(mx, my);
        if (pad >= 0) strike_drum(pad);
        return 0;
    }

    case WM_KEYDOWN:
        for (int i = 0; i < 12; i++) {
            if (w == PAD_KEYS[i]) {
                strike_drum(i);
                return 0;
            }
        }
        // Volume: [ / ] keys
        if (w == VK_OEM_4) { // [
            g_volume = (g_volume > 0) ? g_volume - 1 : 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (w == VK_OEM_6) { // ]
            g_volume = (g_volume < 15) ? g_volume + 1 : 15;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;

    case WM_KEYUP:
        return 0;

    case WM_TIMER:
        if (w == 1 && g_hitTimer) {
            KillTimer(hwnd, g_hitTimer);
            g_hitTimer = 0;
            g_hitPad = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_CTLCOLORSTATIC:
        return (LRESULT)GetStockObject(NULL_BRUSH);

    case WM_DESTROY:
        if (g_hitTimer) KillTimer(hwnd, g_hitTimer);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, w, l);
}

// ============================================================
// WinMain
// ============================================================
int WINAPI WinMain(HINSTANCE hinst, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hp; (void)cmd;
    g_hinst = hinst;

    // Init SPFM
    if (spfm_init(0) != 0) {
        MessageBox(NULL, "Cannot open SPFM device!", "Error", MB_OK | MB_ICONERROR);
        g_connected = 0;
    } else {
        g_connected = 1;
        ym7129_init(g_slot);
        Sleep(100);
    }

    // Register window class
    WNDCLASS wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hinst;
    wc.hbrBackground = NULL;
    wc.lpszClassName = "YM7129DrumPad";
    wc.hCursor       = LoadCursor(NULL, IDC_HAND);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClass(&wc);

    // Create window
    g_hwnd = CreateWindow("YM7129DrumPad", "YM7129 Drum Pad",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, WIN_W, WIN_H,
        NULL, NULL, hinst, NULL);

    // Create fonts
    g_hFontPad    = CreateFont(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
    g_hFontStatus = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
    g_hFontKey    = CreateFont(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");

    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_connected) {
        ym7129_drum_mute(g_slot);
        spfm_cleanup();
    }

    DeleteObject(g_hFontPad);
    DeleteObject(g_hFontStatus);
    DeleteObject(g_hFontKey);

    return 0;
}

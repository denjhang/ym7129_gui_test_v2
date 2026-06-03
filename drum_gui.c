/**
 * YM7129 Drum Pad - Win32 GDI (double-buffered)
 *
 * Pure Win32 GDI drum pad. 12 pads: 6 types x 2 (L/H).
 * Mouse click + numpad keys. Connect/Disconnect/Reset buttons.
 * Delay slider (0-200us) controls per-write hardware delay.
 * Uses verified 3-byte raw protocol: [slot, 0x80, addr] [slot, 0x81, data]
 */

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "spfm.h"
#include "ym7129.h"

#pragma comment(lib, "comctl32.lib")

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

// Key mapping by groups: 0,1 / 2,3 / 4,5 / 7,8 / *
static const UINT PAD_KEYS[12] = {
    '0', '1',                      // BD H/L
    '2', '3',                      // SD H/L
    '4', '5',                      // C.HH H/L
    '7', '8',                      // O.HH H/L
    VK_MULTIPLY, VK_SUBTRACT,      // Tom H/L
    VK_DIVIDE,   VK_ADD,           // Ride H/L
};

// ============================================================
// Control IDs
// ============================================================
#define IDC_BTN_RESET     1001
#define IDC_BTN_CONNECT   1002
#define IDC_BTN_DISCONNECT 1003
#define IDC_TRACKBAR_DELAY 1004
#define IDC_STATIC_DELAY   1005

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
#define COL_BTN_BG     RGB(50, 50, 65)

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

#define CTRL_Y    (PAD_AREA_Y + PAD_AREA_H + 12)
#define CTRL_H    28
#define WIN_W     (PAD_AREA_X * 2 + PAD_AREA_W + 20)
#define WIN_H     (CTRL_Y + CTRL_H + 80)

// ============================================================
// State
// ============================================================
static HINSTANCE g_hinst;
static HWND      g_hwnd;
static HWND      g_hBtnReset;
static HWND      g_hBtnConnect;
static HWND      g_hBtnDisconnect;
static HWND      g_hTrackDelay;
static HWND      g_hStaticDelay;

static HFONT     g_hFontPad;
static HFONT     g_hFontStatus;
static HFONT     g_hFontKey;
static HFONT     g_hFontBtn;

// Double buffer
static HDC       g_memDC = NULL;
static HBITMAP   g_memBmp = NULL;
static int       g_memW = 0, g_memH = 0;

static int      g_connected = 0;
static uint8_t  g_slot = 0;
static int      g_hitPad = -1;
static int      g_hoverPad = -1;
static int      g_volume = 0;
static int      g_delay_us = 80;     // default write delay

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
// Connect / Disconnect / Reset
// ============================================================
static void do_connect(void) {
    if (g_connected) return;
    if (spfm_init(0) != 0) {
        MessageBox(g_hwnd, "Cannot open SPFM device!", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    g_connected = 1;
    ym7129_set_write_delay((uint32_t)g_delay_us);
    ym7129_init(g_slot);
    Sleep(100);
    EnableWindow(g_hBtnConnect, FALSE);
    EnableWindow(g_hBtnDisconnect, TRUE);
    EnableWindow(g_hBtnReset, TRUE);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void do_disconnect(void) {
    if (!g_connected) return;
    ym7129_drum_mute(g_slot);
    spfm_flush();
    Sleep(50);
    spfm_cleanup();
    g_connected = 0;
    EnableWindow(g_hBtnConnect, TRUE);
    EnableWindow(g_hBtnDisconnect, FALSE);
    EnableWindow(g_hBtnReset, FALSE);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void do_reset(void) {
    if (!g_connected) return;
    ym7129_mute(g_slot);
    ym7129_drum_mute(g_slot);
    Sleep(50);
    ym7129_init(g_slot);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ============================================================
// Drawing (to memory DC for double-buffering)
// ============================================================
static void DrawRoundRect(HDC hdc, RECT *r, COLORREF fill, COLORREF border, int radius) {
    HBRUSH hbr = CreateSolidBrush(fill);
    HPEN   hpn = CreatePen(PS_SOLID, 2, border);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, hbr);
    HPEN   oldPn = (HPEN)SelectObject(hdc, hpn);
    RoundRect(hdc, r->left, r->top, r->right, r->bottom, radius, radius);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPn);
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
    const char *keynames[] = {
        "0","1","2","3","4","5",
        "7","8","*","-","/","+"
    };
    char keyhint[8];
    snprintf(keyhint, sizeof(keyhint), "[%s]", keynames[i]);
    RECT kr = { r.right - 52, r.top + 4, r.right - 4, r.bottom - 4 };
    DrawText(hdc, keyhint, -1, &kr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void DrawStatus(HDC hdc, int w, int h) {
    RECT sr = { PAD_AREA_X, h - 28, w - 20, h - 6 };

    const char *status_text;
    COLORREF col;
    if (g_connected) {
        status_text = "Connected";
        col = COL_CONNECTED;
    } else {
        status_text = "Disconnected";
        col = COL_DISCONNECTED;
    }

    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_hFontStatus);
    DrawText(hdc, status_text, -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    char vol[32];
    snprintf(vol, sizeof(vol), "Vol: %d  Delay: %dus", g_volume, g_delay_us);
    RECT vr = { sr.right - 160, sr.top, sr.right, sr.bottom };
    DrawText(hdc, vol, -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void EnsureMemDC(HWND hwnd) {
    RECT cr;
    GetClientRect(hwnd, &cr);
    int w = cr.right, h = cr.bottom;
    if (g_memDC && w == g_memW && h == g_memH) return;

    if (g_memBmp) { DeleteObject(g_memBmp); g_memBmp = NULL; }
    if (g_memDC) { DeleteDC(g_memDC); g_memDC = NULL; }

    HDC hdc = GetDC(hwnd);
    g_memDC = CreateCompatibleDC(hdc);
    g_memBmp = CreateCompatibleBitmap(hdc, w, h);
    SelectObject(g_memDC, g_memBmp);
    ReleaseDC(hwnd, hdc);
    g_memW = w; g_memH = h;
}

static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    EnsureMemDC(hwnd);

    // Draw everything to memory DC
    RECT cr = { 0, 0, g_memW, g_memH };
    HBRUSH bgBr = CreateSolidBrush(COL_BG);
    FillRect(g_memDC, &cr, bgBr);
    DeleteObject(bgBr);

    // Title
    SetTextColor(g_memDC, COL_TEXT);
    SetBkMode(g_memDC, TRANSPARENT);
    SelectObject(g_memDC, g_hFontStatus);
    RECT tr = { PAD_AREA_X, 10, g_memW - 20, 32 };
    DrawText(g_memDC, "YM7129 Drum Pad", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    for (int i = 0; i < 12; i++) DrawPad(g_memDC, i);
    DrawStatus(g_memDC, g_memW, g_memH);

    // Blit to screen (flicker-free)
    BitBlt(hdc, 0, 0, g_memW, g_memH, g_memDC, 0, 0, SRCCOPY);

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

    case WM_SIZE:
        // Invalidate mem buffer on resize
        if (g_memBmp) { DeleteObject(g_memBmp); g_memBmp = NULL; }
        if (g_memDC) { DeleteDC(g_memDC); g_memDC = NULL; }
        g_memW = g_memH = 0;
        return 0;

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

    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_BTN_RESET:
            do_reset();
            break;
        case IDC_BTN_CONNECT:
            do_connect();
            break;
        case IDC_BTN_DISCONNECT:
            do_disconnect();
            break;
        }
        return 0;

    case WM_HSCROLL:
        if ((HWND)l == g_hTrackDelay) {
            g_delay_us = (int)SendMessage(g_hTrackDelay, TBM_GETPOS, 0, 0);
            if (g_connected) ym7129_set_write_delay((uint32_t)g_delay_us);
            char buf[32];
            snprintf(buf, sizeof(buf), "Delay: %d us", g_delay_us);
            SetWindowText(g_hStaticDelay, buf);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_DESTROY:
        if (g_hitTimer) KillTimer(hwnd, g_hitTimer);
        if (g_memBmp) DeleteObject(g_memBmp);
        if (g_memDC) DeleteDC(g_memDC);
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

    // Don't auto-connect — let user click Connect
    g_connected = 0;

    WNDCLASS wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hinst;
    wc.hbrBackground = NULL;
    wc.lpszClassName = "YM7129DrumPad";
    wc.hCursor       = LoadCursor(NULL, IDC_HAND);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClass(&wc);

    g_hwnd = CreateWindow("YM7129DrumPad", "YM7129 Drum Pad",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, WIN_W, WIN_H,
        NULL, NULL, hinst, NULL);

    // Fonts
    g_hFontPad    = CreateFont(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
    g_hFontStatus = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
    g_hFontKey    = CreateFont(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
    g_hFontBtn    = CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");

    // ---- Controls ----
    int cx = PAD_AREA_X;
    int cy = CTRL_Y;

    // Reset button
    g_hBtnReset = CreateWindow("BUTTON", "Reset",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        cx, cy, 60, CTRL_H, g_hwnd, (HMENU)IDC_BTN_RESET, hinst, NULL);
    SendMessage(g_hBtnReset, WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
    EnableWindow(g_hBtnReset, FALSE);
    cx += 68;

    // Connect button
    g_hBtnConnect = CreateWindow("BUTTON", "Connect",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        cx, cy, 68, CTRL_H, g_hwnd, (HMENU)IDC_BTN_CONNECT, hinst, NULL);
    SendMessage(g_hBtnConnect, WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
    cx += 76;

    // Disconnect button
    g_hBtnDisconnect = CreateWindow("BUTTON", "Disconnect",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        cx, cy, 80, CTRL_H, g_hwnd, (HMENU)IDC_BTN_DISCONNECT, hinst, NULL);
    SendMessage(g_hBtnDisconnect, WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
    EnableWindow(g_hBtnDisconnect, FALSE);
    cy += CTRL_H + 8;

    // Delay label
    cx = PAD_AREA_X;
    CreateWindow("STATIC", "Write Delay:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        cx, cy, 76, 20, g_hwnd, NULL, hinst, NULL);
    SendMessage(GetWindow(g_hwnd, GW_CHILD), WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
    cx += 82;

    // Delay trackbar (0-200us)
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    g_hTrackDelay = CreateWindow(TRACKBAR_CLASS, "",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS | TBS_TOOLTIPS,
        cx, cy - 4, 160, 28, g_hwnd, (HMENU)IDC_TRACKBAR_DELAY, hinst, NULL);
    SendMessage(g_hTrackDelay, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
    SendMessage(g_hTrackDelay, TBM_SETPOS, TRUE, 80);
    SendMessage(g_hTrackDelay, TBM_SETTICFREQ, 0, 20);
    cx += 168;

    // Delay value label
    char delayBuf[32];
    snprintf(delayBuf, sizeof(delayBuf), "Delay: %d us", g_delay_us);
    g_hStaticDelay = CreateWindow("STATIC", delayBuf,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        cx, cy, 80, 20, g_hwnd, (HMENU)IDC_STATIC_DELAY, hinst, NULL);
    SendMessage(g_hStaticDelay, WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);

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
    DeleteObject(g_hFontBtn);

    return 0;
}

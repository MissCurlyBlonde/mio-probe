/*
 * C710auto.c  (v7, pink edition: ActiveSync keep-alive, hardware home key, draggable home button)
 * Touch launcher, photo frame and clock for the Mio DigiWalker C710.
 * Windows CE .NET 4.2, ARM, 320x240, plain GDI only (no DirectDraw, no image decoders).
 *
 * Put the built C710auto.exe in the ROOT of the SD card. Mio's ST.exe starts it at boot
 * (and hides the taskbar, so this program is the only thing on screen).
 *
 * Photo frame:  BMP files (uncompressed 16/24/32-bit) in \Storage Card\Frame
 *               (fallback: \My Flash Disk\Frame). 320x240 is ideal; other sizes are
 *               scaled to fit.
 * Extra tiles:  optional text file \Storage Card\launcher.txt, one tile per line:
 *                   Name|\path\to\program.exe|optional arguments
 *               lines starting with # are ignored. Each gets a coloured tile with
 *               the first letter of its name as the icon.
 *
 * PC screen:    tile "PC screen" shows what c710_display.py (on the PC) sends over the
 *               USB sync cable. Frames travel as \Temp\c710frame.bmp.
 *
 * Settings:     optional lines in \Storage Card\launcher.txt
 *                   @home=tr      where the floating home button sits: tl tr bl br tc bc
 *                   @video=\Windows\SomePlayer.exe    force a specific video player
 *                   @homebutton=off   hide the on-screen home button (use the hardware key)
 *                   @homekey=0x5B     hardware key for "home" (or use the Buttons tile to learn it)
 *                   @activesync=/remote   arguments for repllog.exe when the launcher restarts it
 *
 * Controls:     tap a tile to open it, swipe left/right (or tap the arrows) to change page.
 *               Clock: tap anywhere to go back.
 *               Photos: tap left/right third = previous/next, middle = show/hide clock,
 *               top-left corner = back.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <string.h>

#define SCR_W      320
#define SCR_H      240
#define BAR_H      30
#define FOOT_H     22
#define GAP        6
#define COLS       3
#define ROWS       2
#define PER_PAGE   (COLS * ROWS)
#define MAX_TILES  40
#define MAX_FILES  400
#define FRAME_SECS 10
#define ID_TICK    1
#define ID_REPEAT  2
#define ID_POLL    3
#define ID_KEY     4
#define HOME_W     48                   /* the always-on-top "back to launcher" button */
#define HOME_H     30
#define MAX_VID    150
#define VROWS      5
#define PC_FRAME   L"\\Temp\\c710frame.bmp"   /* written by c710_display.py over USB */
#define NONE       ((COLORREF)0xFFFFFFFF)

/* palette */
#define BG_TOP     RGB(62, 22, 52)
#define BG_BOT     RGB(128, 52, 104)
#define C_TEXT     RGB(255, 248, 252)
#define C_DIM      RGB(228, 184, 208)
#define C_ACCENT   RGB(255, 196, 214)

enum { M_MENU, M_CLOCK, M_FRAME, M_SETTIME, M_PC, M_VIDEO, M_KEYS };
enum { A_FRAME, A_CLOCK, A_SETTIME, A_TASKBAR, A_DESKTOP, A_PC, A_VIDEO, A_SYNC, A_KEYS, A_RUN };
enum { I_PHOTO, I_CLOCK, I_CAL, I_FOLDER, I_BAR, I_HOME, I_GRID, I_MUSIC, I_MON, I_VIDEO, I_SYNC, I_KEYS, I_APP };

typedef struct {
    WCHAR name[28];
    WCHAR path[100];
    WCHAR args[100];
    int action;
    int icon;
    int color;
} Tile;

/* tile colours: top and bottom of the gradient */
#define NCOLORS 8
static const COLORREF g_ctop[NCOLORS] = {
    RGB(255, 112, 172), RGB(247, 134, 164), RGB(255, 168, 146), RGB(214, 132, 232),
    RGB(192, 148, 182), RGB(255, 154, 198), RGB(238, 96, 154), RGB(250, 200, 166)
};
static const COLORREF g_cbot[NCOLORS] = {
    RGB(192, 42, 122), RGB(188, 62, 102), RGB(208, 98, 88), RGB(142, 68, 172),
    RGB(118, 78, 112), RGB(208, 78, 142), RGB(164, 32, 98), RGB(204, 138, 104)
};

/* ---- state ---- */
static Tile g_tiles[MAX_TILES];
static int g_ntiles = 0;
static int g_page = 0;
static int g_mode = M_MENU;
static int g_pressed = -1;
static int g_downx = 0, g_downy = 0;
static int g_lastmin = -1;
static HWND g_wnd = NULL;

static HDC g_back = NULL;
static HBITMAP g_backbmp = NULL;
static HGDIOBJ g_backold = NULL;
static HDC g_bgdc = NULL;               /* cached gradient background */
static HBITMAP g_bgbmp = NULL;
static HGDIOBJ g_bgold = NULL;
static HFONT f_small, f_label, f_med, f_big, f_date, f_over, f_letter, f_clk;

/* photo frame */
static WCHAR g_files[MAX_FILES][72];
static int g_nfiles = 0;
static int g_cur = 0;
static WCHAR g_dir[80];
static HBITMAP g_img = NULL;
static HDC g_imgdc = NULL;
static HGDIOBJ g_imgold = NULL;
static int g_iw = 0, g_ih = 0;
static int g_secs = 0;
static BOOL g_showclock = TRUE;

/* "home" button that floats over other apps */
static HWND g_home = NULL;
static BOOL g_home_down = FALSE;
static BOOL g_child_active = FALSE;
static DWORD g_child_tick = 0;
static int g_home_x = 0, g_home_y = 0, g_rekill = 0;
static WCHAR g_child_name[40];
static int g_homepos = 5;              /* 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right, 4 top-centre, 5 bottom-centre */
static WCHAR g_videoapp[100];          /* optional @video= override from launcher.txt */
static BOOL g_homebtn_on = TRUE;       /* @homebutton=off hides the on-screen button */
static BOOL g_home_custom = FALSE;     /* position was set by dragging the button */
static BOOL g_dragging = FALSE;
static int g_drag_sx = 0, g_drag_sy = 0, g_drag_ox = 0, g_drag_oy = 0;

/* hardware button ("home key") */
static int g_homekey = 0;              /* virtual-key code that means "go home", 0 = none */
static BOOL g_keyprev = FALSE, g_learning = FALSE;
static BYTE g_kbase[256];
static WCHAR g_khist[7][40];
static int g_nkh = 0;

/* ActiveSync keep-alive and little status message */
static WCHAR g_as_args[24];            /* arguments for repllog.exe (last ActiveSync tile used) */
static int g_tickn = 0;
static BOOL g_sync_up = FALSE;
static DWORD g_lastrestart = 0;
static WCHAR g_toast[48];
static DWORD g_toast_until = 0;

/* video list */
typedef struct {
    WCHAR name[44];
    WCHAR path[100];
    DWORD mb;
} VidFile;
static VidFile g_vid[MAX_VID];
static int g_nvid = 0, g_vtop = 0, g_vpress = -1;

/* PC companion display */
static BYTE g_patchbuf[SCR_W * SCR_H * 2];
static BYTE *g_cbits = NULL;            /* pixels of the persistent 320x240 canvas */
static BOOL g_cfull = FALSE;            /* canvas has received a full picture */
static BOOL g_gotframe = FALSE;
static BOOL g_nosig = FALSE;
static DWORD g_lastframe = 0;

/* set-time screen */
static int g_f[5];                 /* day, month, year, hour, minute */
static const int g_fmin[5] = { 1, 1, 2006, 0, 0 };
static const int g_fmax[5] = { 31, 12, 2099, 23, 59 };
static int g_rep_field = 0, g_rep_dir = 0;

static const WCHAR *g_days[7] = {
    L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"
};
static const WCHAR *g_days3[7] = {
    L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"
};
static const WCHAR *g_months[12] = {
    L"January", L"February", L"March", L"April", L"May", L"June",
    L"July", L"August", L"September", L"October", L"November", L"December"
};
static const WCHAR *g_months3[12] = {
    L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
    L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"
};

/* ---- small helpers ---- */
static DWORD rd16(const BYTE *p) { return (DWORD)p[0] | ((DWORD)p[1] << 8); }
static DWORD rd32(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static void signal_evt(const WCHAR *name)
{
    /* Mio's ST.exe watches these named events; Mio's own players use them to stop
       the device from suspending while something is playing. */
    HANDLE h = CreateEvent(NULL, FALSE, FALSE, name);
    if (h) {
        SetEvent(h);
        CloseHandle(h);
    }
}

static HFONT mkfont(int px, int bold)
{
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = DEFAULT_QUALITY;
    lstrcpy(lf.lfFaceName, L"Tahoma");
    return CreateFontIndirect(&lf);
}

static int iabs(int v) { return v < 0 ? -v : v; }

static COLORREF lerp(COLORREF a, COLORREF b, int n, int d)
{
    int r = GetRValue(a) + (GetRValue(b) - GetRValue(a)) * n / d;
    int g = GetGValue(a) + (GetGValue(b) - GetGValue(a)) * n / d;
    int bl = GetBValue(a) + (GetBValue(b) - GetBValue(a)) * n / d;
    return RGB(r, g, bl);
}

static BOOL starts_with(const WCHAR *s, const WCHAR *p)
{
    while (*p) {
        if (*s++ != *p++)
            return FALSE;
    }
    return TRUE;
}

static const WCHAR *base_name(const WCHAR *p)
{
    const WCHAR *b = p, *q;
    for (q = p; *q; q++) {
        if (*q == L'\\' || *q == L'/')
            b = q + 1;
    }
    return b;
}

/* tiny debug log on the SD card: \Storage Card\launcher_log.txt */
static void log_line(const WCHAR *s)
{
    char a[300];
    int n;
    DWORD put = 0;
    HANDLE h;
    n = WideCharToMultiByte(CP_ACP, 0, s, -1, a, 296, NULL, NULL);
    if (n <= 0)
        return;
    a[n - 1] = '\r';
    a[n] = '\n';
    h = CreateFile(L"\\Storage Card\\launcher_log.txt", GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                   FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, a, (DWORD)(n + 1), &put, NULL);
    CloseHandle(h);
}

/* fast solid rectangle without creating a brush */
static void box(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc;
    rc.left = l; rc.top = t; rc.right = r; rc.bottom = b;
    SetBkColor(dc, c);
    ExtTextOut(dc, l, t, ETO_OPAQUE, &rc, NULL, 0, NULL);
}

static void text(HDC dc, HFONT f, COLORREF c, const WCHAR *s,
                 int l, int t, int r, int b, UINT fmt)
{
    RECT rc;
    rc.left = l; rc.top = t; rc.right = r; rc.bottom = b;
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawText(dc, s, -1, &rc, fmt);
}

static void shadowed(HDC dc, HFONT f, COLORREF c, const WCHAR *s,
                     int l, int t, int r, int b, UINT fmt)
{
    text(dc, f, RGB(0, 0, 0), s, l + 1, t + 1, r + 1, b + 1, fmt);
    text(dc, f, c, s, l, t, r, b, fmt);
}

/* ---- shape drawing ---- */
typedef struct {
    HGDIOBJ op, ob, np, nb;
    int ownp, ownb;
} Ink;

static void ink_on(HDC dc, Ink *k, COLORREF fill, COLORREF line, int lw)
{
    if (line == NONE) {
        k->np = GetStockObject(NULL_PEN);
        k->ownp = 0;
    } else {
        k->np = CreatePen(PS_SOLID, lw < 1 ? 1 : lw, line);
        k->ownp = 1;
    }
    if (fill == NONE) {
        k->nb = GetStockObject(NULL_BRUSH);
        k->ownb = 0;
    } else {
        k->nb = CreateSolidBrush(fill);
        k->ownb = 1;
    }
    k->op = SelectObject(dc, k->np);
    k->ob = SelectObject(dc, k->nb);
}

static void ink_off(HDC dc, Ink *k)
{
    SelectObject(dc, k->op);
    SelectObject(dc, k->ob);
    if (k->ownp) DeleteObject(k->np);
    if (k->ownb) DeleteObject(k->nb);
}

static void rrect(HDC dc, int l, int t, int r, int b, int rad, COLORREF fill, COLORREF line)
{
    Ink k;
    ink_on(dc, &k, fill, line, 1);
    RoundRect(dc, l, t, r, b, rad * 2, rad * 2);
    ink_off(dc, &k);
}

static void ell(HDC dc, int l, int t, int r, int b, COLORREF fill, COLORREF line)
{
    Ink k;
    ink_on(dc, &k, fill, line, 1);
    Ellipse(dc, l, t, r, b);
    ink_off(dc, &k);
}

static void poly(HDC dc, const POINT *p, int n, COLORREF fill)
{
    Ink k;
    ink_on(dc, &k, fill, NONE, 1);
    Polygon(dc, p, n);
    ink_off(dc, &k);
}

static void seg(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int w)
{
    Ink k;
    ink_on(dc, &k, NONE, c, w);
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
    ink_off(dc, &k);
}

/* rounded rectangle with a vertical gradient (radius 8, drawn row by row) */
static void grad_rrect(HDC dc, int l, int t, int r, int b, COLORREF c1, COLORREF c2)
{
    static const int inset[8] = { 5, 3, 2, 1, 1, 0, 0, 0 };
    int h = b - t, y;
    for (y = 0; y < h; y++) {
        int in = 0;
        if (y < 8) in = inset[y];
        else if (h - 1 - y < 8) in = inset[h - 1 - y];
        box(dc, l + in, t + y, r - in, t + y + 1, lerp(c1, c2, y, h > 1 ? h - 1 : 1));
    }
}

/* ---- icons: drawn in a roughly 40x40 box centred on cx, cy ---- */
static void icon_photo(HDC dc, int cx, int cy, COLORREF dk)
{
    POINT m[5];
    rrect(dc, cx - 19, cy - 15, cx + 19, cy + 15, 4, RGB(255, 255, 255), NONE);
    box(dc, cx - 15, cy - 11, cx + 15, cy + 11, dk);
    ell(dc, cx + 3, cy - 9, cx + 11, cy - 1, RGB(255, 236, 160), NONE);
    m[0].x = cx - 15; m[0].y = cy + 11;
    m[1].x = cx - 6;  m[1].y = cy - 2;
    m[2].x = cx + 1;  m[2].y = cy + 6;
    m[3].x = cx + 7;  m[3].y = cy + 1;
    m[4].x = cx + 15; m[4].y = cy + 11;
    poly(dc, m, 5, RGB(236, 242, 252));
}

static void icon_clock(HDC dc, int cx, int cy, COLORREF dk)
{
    ell(dc, cx - 19, cy - 19, cx + 19, cy + 19, RGB(255, 255, 255), NONE);
    ell(dc, cx - 15, cy - 15, cx + 15, cy + 15, dk, NONE);
    seg(dc, cx, cy - 13, cx, cy - 10, RGB(255, 255, 255), 2);
    seg(dc, cx, cy + 10, cx, cy + 13, RGB(255, 255, 255), 2);
    seg(dc, cx - 13, cy, cx - 10, cy, RGB(255, 255, 255), 2);
    seg(dc, cx + 10, cy, cx + 13, cy, RGB(255, 255, 255), 2);
    seg(dc, cx, cy, cx, cy - 9, RGB(255, 255, 255), 2);
    seg(dc, cx, cy, cx + 7, cy + 4, RGB(255, 255, 255), 2);
    ell(dc, cx - 2, cy - 2, cx + 3, cy + 3, RGB(255, 255, 255), NONE);
}

static void icon_cal(HDC dc, int cx, int cy, COLORREF dk)
{
    int r, c;
    rrect(dc, cx - 18, cy - 16, cx + 18, cy + 18, 4, RGB(255, 255, 255), NONE);
    rrect(dc, cx - 18, cy - 16, cx + 18, cy - 4, 4, dk, NONE);
    box(dc, cx - 18, cy - 9, cx + 18, cy - 4, dk);
    seg(dc, cx - 9, cy - 20, cx - 9, cy - 12, RGB(255, 255, 255), 3);
    seg(dc, cx + 9, cy - 20, cx + 9, cy - 12, RGB(255, 255, 255), 3);
    for (r = 0; r < 2; r++)
        for (c = 0; c < 3; c++)
            box(dc, cx - 13 + c * 10, cy + 1 + r * 9, cx - 13 + c * 10 + 7, cy + 1 + r * 9 + 6, dk);
}

static void icon_folder(HDC dc, int cx, int cy, COLORREF lt)
{
    rrect(dc, cx - 19, cy - 15, cx - 1, cy - 4, 3, RGB(255, 255, 255), NONE);
    rrect(dc, cx - 19, cy - 8, cx + 19, cy + 16, 4, RGB(255, 255, 255), NONE);
    rrect(dc, cx - 19, cy - 3, cx + 19, cy + 16, 4, lt, NONE);
    box(dc, cx - 19, cy - 3, cx + 19, cy + 2, lt);
}

static void icon_bar(HDC dc, int cx, int cy, COLORREF dk, COLORREF lt)
{
    rrect(dc, cx - 19, cy - 15, cx + 19, cy + 15, 4, RGB(255, 255, 255), NONE);
    box(dc, cx - 15, cy - 11, cx + 15, cy + 7, dk);
    box(dc, cx - 15, cy + 8, cx + 15, cy + 11, lt);
    box(dc, cx - 14, cy + 9, cx - 10, cy + 11, RGB(255, 255, 255));
    box(dc, cx - 11, cy - 8, cx + 1, cy - 2, lt);
}

static void icon_home(HDC dc, int cx, int cy, COLORREF dk)
{
    POINT m[3];
    m[0].x = cx - 21; m[0].y = cy - 1;
    m[1].x = cx;      m[1].y = cy - 19;
    m[2].x = cx + 21; m[2].y = cy - 1;
    poly(dc, m, 3, RGB(255, 255, 255));
    rrect(dc, cx - 15, cy - 3, cx + 15, cy + 17, 3, RGB(255, 255, 255), NONE);
    box(dc, cx - 4, cy + 5, cx + 5, cy + 17, dk);
}

static void icon_grid(HDC dc, int cx, int cy, COLORREF lt)
{
    rrect(dc, cx - 18, cy - 18, cx - 2, cy - 2, 4, RGB(255, 255, 255), NONE);
    rrect(dc, cx + 2,  cy - 18, cx + 18, cy - 2, 4, RGB(255, 255, 255), NONE);
    rrect(dc, cx - 18, cy + 2,  cx - 2,  cy + 18, 4, RGB(255, 255, 255), NONE);
    rrect(dc, cx + 2,  cy + 2,  cx + 18, cy + 18, 4, lt, NONE);
}

static void icon_music(HDC dc, int cx, int cy)
{
    POINT m[4];
    COLORREF w = RGB(255, 255, 255);
    ell(dc, cx - 16, cy + 5, cx - 4, cy + 15, w, NONE);
    ell(dc, cx + 3, cy + 2, cx + 15, cy + 12, w, NONE);
    seg(dc, cx - 5, cy + 10, cx - 5, cy - 14, w, 3);
    seg(dc, cx + 14, cy + 7, cx + 14, cy - 17, w, 3);
    m[0].x = cx - 5;  m[0].y = cy - 14;
    m[1].x = cx + 14; m[1].y = cy - 18;
    m[2].x = cx + 14; m[2].y = cy - 10;
    m[3].x = cx - 5;  m[3].y = cy - 6;
    poly(dc, m, 4, w);
}

static void icon_monitor(HDC dc, int cx, int cy, COLORREF dk, COLORREF lt)
{
    rrect(dc, cx - 20, cy - 17, cx + 20, cy + 9, 4, RGB(255, 255, 255), NONE);
    box(dc, cx - 16, cy - 13, cx + 16, cy + 5, dk);
    box(dc, cx - 12, cy - 9, cx - 1, cy - 2, lt);
    box(dc, cx + 2, cy - 9, cx + 12, cy - 5, lt);
    box(dc, cx - 3, cy + 9, cx + 4, cy + 14, RGB(255, 255, 255));
    rrect(dc, cx - 11, cy + 14, cx + 11, cy + 18, 2, RGB(255, 255, 255), NONE);
}

static void icon_video(HDC dc, int cx, int cy, COLORREF dk)
{
    POINT m[3];
    rrect(dc, cx - 20, cy - 15, cx + 20, cy + 15, 5, RGB(255, 255, 255), NONE);
    box(dc, cx - 16, cy - 11, cx + 16, cy + 11, dk);
    m[0].x = cx - 5; m[0].y = cy - 8;
    m[1].x = cx - 5; m[1].y = cy + 8;
    m[2].x = cx + 9; m[2].y = cy;
    poly(dc, m, 3, RGB(255, 255, 255));
}

static void icon_sync(HDC dc, int cx, int cy)
{
    POINT a[3], b[3];
    Ink k;
    ink_on(dc, &k, NONE, RGB(255, 255, 255), 4);
    Ellipse(dc, cx - 14, cy - 14, cx + 14, cy + 14);
    ink_off(dc, &k);
    a[0].x = cx + 8;  a[0].y = cy - 21;
    a[1].x = cx + 20; a[1].y = cy - 11;
    a[2].x = cx + 6;  a[2].y = cy - 7;
    poly(dc, a, 3, RGB(255, 255, 255));
    b[0].x = cx - 8;  b[0].y = cy + 21;
    b[1].x = cx - 20; b[1].y = cy + 11;
    b[2].x = cx - 6;  b[2].y = cy + 7;
    poly(dc, b, 3, RGB(255, 255, 255));
}

static void icon_keys(HDC dc, int cx, int cy, COLORREF dk)
{
    rrect(dc, cx - 20, cy - 15, cx + 20, cy + 15, 6, RGB(255, 255, 255), NONE);
    box(dc, cx - 15, cy - 10, cx + 5, cy + 10, dk);
    ell(dc, cx + 9, cy - 4, cx + 17, cy + 4, dk, NONE);
    ell(dc, cx + 11, cy - 2, cx + 15, cy + 2, RGB(255, 255, 255), NONE);
}

static void icon_app(HDC dc, int cx, int cy, COLORREF dk, const WCHAR *name)
{
    WCHAR s[2];
    s[0] = name[0] ? name[0] : L'?';
    s[1] = 0;
    rrect(dc, cx - 18, cy - 18, cx + 18, cy + 18, 8, RGB(255, 255, 255), NONE);
    text(dc, f_letter, dk, s, cx - 18, cy - 18, cx + 18, cy + 18,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void draw_icon(HDC dc, const Tile *t, int cx, int cy)
{
    COLORREF dk = g_cbot[t->color];
    COLORREF lt = lerp(g_ctop[t->color], RGB(255, 255, 255), 55, 100);
    switch (t->icon) {
    case I_PHOTO:  icon_photo(dc, cx, cy, dk); break;
    case I_CLOCK:  icon_clock(dc, cx, cy, dk); break;
    case I_CAL:    icon_cal(dc, cx, cy, dk); break;
    case I_FOLDER: icon_folder(dc, cx, cy, lt); break;
    case I_BAR:    icon_bar(dc, cx, cy, dk, lt); break;
    case I_HOME:   icon_home(dc, cx, cy, dk); break;
    case I_GRID:   icon_grid(dc, cx, cy, lt); break;
    case I_MUSIC:  icon_music(dc, cx, cy); break;
    case I_MON:    icon_monitor(dc, cx, cy, dk, lt); break;
    case I_VIDEO:  icon_video(dc, cx, cy, dk); break;
    case I_SYNC:   icon_sync(dc, cx, cy); break;
    case I_KEYS:   icon_keys(dc, cx, cy, dk); break;
    default:       icon_app(dc, cx, cy, dk, t->name); break;
    }
}

static void set_mode(int m);
static void draw_bg(HDC dc);
static void draw_topbar(HDC dc, const WCHAR *title);

static void redraw(void)
{
    if (g_wnd)
        InvalidateRect(g_wnd, NULL, FALSE);
}

static void toast(const WCHAR *s)
{
    lstrcpyn(g_toast, s, 48);
    g_toast_until = GetTickCount() + 3500;
    redraw();
}

static int parse_num(const WCHAR *s)
{
    int v = 0;
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) {
        for (s += 2; *s; s++) {
            if (*s >= L'0' && *s <= L'9') v = v * 16 + (*s - L'0');
            else if (*s >= L'a' && *s <= L'f') v = v * 16 + (*s - L'a' + 10);
            else if (*s >= L'A' && *s <= L'F') v = v * 16 + (*s - L'A' + 10);
            else break;
        }
    } else {
        for (; *s >= L'0' && *s <= L'9'; s++)
            v = v * 10 + (*s - L'0');
    }
    return v;
}

static int read_ascii(const WCHAR *path, char *buf, int n)
{
    HANDLE h;
    DWORD got = 0;
    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                   FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(h, buf, n - 1, &got, NULL))
        got = 0;
    CloseHandle(h);
    buf[got] = 0;
    return (int)got;
}

static void write_ascii(const WCHAR *path, const char *s)
{
    HANDLE h;
    DWORD put = 0;
    h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, s, (DWORD)strlen(s), &put, NULL);
    CloseHandle(h);
}

static void num_to_ascii(int v, char *out)
{
    char t[16];
    int n = 0, i = 0;
    if (v < 0)
        v = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0 && n < 14);
    while (n > 0)
        out[i++] = t[--n];
    out[i] = 0;
}

static const char *ascii_int(const char *p, int *v)
{
    *v = 0;
    while (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n')
        p++;
    while (*p >= '0' && *p <= '9')
        *v = *v * 10 + (*p++ - '0');
    return p;
}

static void save_homekey(void)
{
    char b[24];
    num_to_ascii(g_homekey, b);
    write_ascii(L"\\Storage Card\\launcher_key.txt", b);
}

static void save_homepos(void)
{
    char a[16], b[16], out[40];
    num_to_ascii(g_home_x, a);
    num_to_ascii(g_home_y, b);
    strcpy(out, a);
    strcat(out, " ");
    strcat(out, b);
    write_ascii(L"\\Storage Card\\launcher_pos.txt", out);
}

/* remembered settings written by the launcher itself */
static void load_saved(void)
{
    char b[48];
    const char *p;
    int v, v2;
    if (read_ascii(L"\\Storage Card\\launcher_key.txt", b, sizeof(b)) > 0) {
        ascii_int(b, &v);
        if (v > 0 && v < 255)
            g_homekey = v;
    }
    if (read_ascii(L"\\Storage Card\\launcher_pos.txt", b, sizeof(b)) > 0) {
        p = ascii_int(b, &v);
        ascii_int(p, &v2);
        if (v >= 0 && v2 >= 0 && v <= SCR_W - HOME_W && v2 <= SCR_H - HOME_H) {
            g_home_x = v;
            g_home_y = v2;
            g_home_custom = TRUE;
        }
    }
}

/* ---- tiles ---- */
static void add_tile(const WCHAR *name, const WCHAR *path, const WCHAR *args,
                     int action, int icon, int color)
{
    Tile *t;
    if (g_ntiles >= MAX_TILES)
        return;
    t = &g_tiles[g_ntiles++];
    lstrcpyn(t->name, name, 28);
    lstrcpyn(t->path, path ? path : L"", 100);
    lstrcpyn(t->args, args ? args : L"", 100);
    t->action = action;
    t->icon = icon;
    t->color = color % NCOLORS;
}

static void load_config(void)
{
    static char buf[8192];
    static WCHAR w[8192];
    HANDLE h;
    DWORD got = 0;
    int n, i;

    h = CreateFile(L"\\Storage Card\\launcher.txt", GENERIC_READ, FILE_SHARE_READ,
                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL))
        got = 0;
    CloseHandle(h);
    if (got == 0)
        return;
    n = MultiByteToWideChar(CP_ACP, 0, buf, (int)got, w, 8191);
    if (n <= 0)
        return;
    w[n] = 0;

    i = 0;
    while (i < n) {
        WCHAR *line = w + i;
        WCHAR *e = line;
        WCHAR *p1 = NULL, *p2 = NULL, *q;

        while (*e && *e != L'\n' && *e != L'\r')
            e++;
        if (*e) {
            *e = 0;
            i = (int)(e - w) + 1;
        } else {
            i = n;
        }
        for (q = line; *q; q++) {
            if (*q == L'|') {
                if (!p1) p1 = q;
                else if (!p2) p2 = q;
            }
        }
        if (starts_with(line, L"@home=")) {
            const WCHAR *v = line + 6;
            if (!lstrcmpi(v, L"tl")) g_homepos = 0;
            else if (!lstrcmpi(v, L"tr")) g_homepos = 1;
            else if (!lstrcmpi(v, L"bl")) g_homepos = 2;
            else if (!lstrcmpi(v, L"br")) g_homepos = 3;
            else if (!lstrcmpi(v, L"tc")) g_homepos = 4;
            else if (!lstrcmpi(v, L"bc")) g_homepos = 5;
            continue;
        }
        if (starts_with(line, L"@homekey=")) {
            int v = parse_num(line + 9);
            if (v > 0 && v < 255)
                g_homekey = v;
            continue;
        }
        if (starts_with(line, L"@homebutton=")) {
            g_homebtn_on = (lstrcmpi(line + 12, L"off") != 0 && lstrcmpi(line + 12, L"0") != 0);
            continue;
        }
        if (starts_with(line, L"@activesync=")) {
            lstrcpyn(g_as_args, line + 12, 24);
            continue;
        }
        if (starts_with(line, L"@video=")) {
            lstrcpyn(g_videoapp, line + 7, 100);
            continue;
        }
        if (line[0] == L'#' || !p1)
            continue;
        *p1 = 0;
        if (p2) *p2 = 0;
        add_tile(line, p1 + 1, p2 ? p2 + 1 : NULL, A_RUN, I_APP, g_ntiles);
    }
}

static void build_tiles(void)
{
    add_tile(L"Photos",   NULL, NULL, A_FRAME,   I_PHOTO,  5);
    add_tile(L"Videos",   NULL, NULL, A_VIDEO,   I_VIDEO,  0);
    add_tile(L"Music",    L"\\Windows\\MP3Player.exe", NULL, A_RUN, I_MUSIC, 6);
    add_tile(L"Clock",    NULL, NULL, A_CLOCK,   I_CLOCK,  1);
    add_tile(L"PC screen", NULL, NULL, A_PC,     I_MON,    3);
    add_tile(L"ActiveSync", NULL, L"",        A_SYNC, I_SYNC, 7);
    add_tile(L"Sync (remote)", NULL, L"/remote", A_SYNC, I_SYNC, 7);
    add_tile(L"Set time", NULL, NULL, A_SETTIME, I_CAL,    2);
    add_tile(L"Files",    L"\\Windows\\explorer.exe", L"\\Storage Card", A_RUN, I_FOLDER, 4);
    add_tile(L"Taskbar",  NULL, NULL, A_TASKBAR, I_BAR,    1);
    add_tile(L"Desktop",  NULL, NULL, A_DESKTOP, I_HOME,   3);
    add_tile(L"Buttons",  NULL, NULL, A_KEYS,    I_KEYS,   6);
    load_config();
}

static void tile_rect(int slot, RECT *rc)
{
    int col = slot % COLS, row = slot / COLS;
    int w = (SCR_W - GAP * (COLS + 1)) / COLS;
    int h = (SCR_H - BAR_H - FOOT_H - GAP * (ROWS + 1)) / ROWS;
    rc->left = GAP + col * (w + GAP);
    rc->top = BAR_H + GAP + row * (h + GAP);
    rc->right = rc->left + w;
    rc->bottom = rc->top + h;
}

static int tile_at(int x, int y)
{
    int slot;
    RECT rc;
    for (slot = 0; slot < PER_PAGE; slot++) {
        int idx = g_page * PER_PAGE + slot;
        if (idx >= g_ntiles)
            break;
        tile_rect(slot, &rc);
        if (x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom)
            return idx;
    }
    return -1;
}

static int page_count(void)
{
    return (g_ntiles + PER_PAGE - 1) / PER_PAGE;
}

static void change_page(int d)
{
    int n = page_count();
    if (n > 1)
        g_page = (g_page + d + n) % n;
}

/* ---- the floating "home" button and app killing ---- */
typedef struct {
    DWORD dwSize, cntUsage, th32ProcessID, th32DefaultHeapID, th32ModuleID, cntThreads,
          th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    WCHAR szExeFile[MAX_PATH];
    DWORD th32MemoryBase, th32AccessKey;
} PE32W;
typedef HANDLE (WINAPI *PFN_SNAP)(DWORD, DWORD);
typedef BOOL (WINAPI *PFN_PROC)(HANDLE, PE32W *);
typedef BOOL (WINAPI *PFN_CLOSESNAP)(HANDLE);

static BOOL protected_name(const WCHAR *n)
{
    static const WCHAR *p[] = { L"explorer.exe", L"ST.exe", L"gwes.exe", L"device.exe",
        L"filesys.exe", L"nk.exe", L"services.exe", L"rapisrv.exe", L"repllog.exe",
        L"BlueSoleil.exe", L"MioAlert.exe", L"C710auto.exe", NULL };
    int i;
    for (i = 0; p[i]; i++) {
        if (!lstrcmpi(n, p[i]))
            return TRUE;
    }
    return FALSE;
}

/* counts (and optionally closes) running programs: the names in list, plus "extra" */
static int scan_procs(const WCHAR *const *list, const WCHAR *extra, BOOL kill)
{
    HMODULE lib;
    PFN_SNAP snap;
    PFN_PROC first, next;
    PFN_CLOSESNAP closesnap;
    HANDLE sh;
    PE32W pe;
    BOOL ok;
    int count = 0;

    lib = LoadLibrary(L"toolhelp.dll");
    if (!lib)
        return 0;
    snap = (PFN_SNAP)GetProcAddress(lib, (void *)L"CreateToolhelp32Snapshot");
    first = (PFN_PROC)GetProcAddress(lib, (void *)L"Process32First");
    next = (PFN_PROC)GetProcAddress(lib, (void *)L"Process32Next");
    closesnap = (PFN_CLOSESNAP)GetProcAddress(lib, (void *)L"CloseToolhelp32Snapshot");
    if (snap && first && next) {
        sh = snap(0x2, 0);                      /* TH32CS_SNAPPROCESS */
        if (sh != INVALID_HANDLE_VALUE && sh != NULL) {
            memset(&pe, 0, sizeof(pe));
            pe.dwSize = sizeof(pe);
            ok = first(sh, &pe);
            while (ok) {
                BOOL match = FALSE;
                int i;
                for (i = 0; list && list[i]; i++) {
                    if (!lstrcmpi(pe.szExeFile, list[i]))
                        match = TRUE;
                }
                if (extra && extra[0] && !lstrcmpi(pe.szExeFile, extra) &&
                    !protected_name(pe.szExeFile))
                    match = TRUE;
                if (match) {
                    count++;
                    if (kill) {
                        HANDLE ph = OpenProcess(0, FALSE, pe.th32ProcessID);
                        if (ph) {
                            TerminateProcess(ph, 0);
                            CloseHandle(ph);
                        }
                    }
                }
                pe.dwSize = sizeof(pe);
                ok = next(sh, &pe);
            }
            if (closesnap)
                closesnap(sh);
            else
                CloseHandle(sh);
        }
    }
    FreeLibrary(lib);
    return count;
}

/* closes the programs the launcher starts (and the one we just launched) */
static void kill_apps(void)
{
    static const WCHAR *list[] = { L"MainShell.exe", L"MP3Player.exe", L"PictureViewer.exe",
        L"MioUtility.exe", L"Contact.exe", L"Calculator.exe",
        L"PLAYER.EXE", L"playerce5.exe", L"TCPMP.EXE", L"videoplayer.exe", L"coreplayer.exe",
        NULL };
    scan_procs(list, g_child_name, TRUE);
}

/* ActiveSync (repllog.exe) is the device end of the USB link to Windows Mobile Device Center */
static BOOL sync_running(void)
{
    static const WCHAR *l[] = { L"repllog.exe", NULL };
    return scan_procs(l, NULL, FALSE) > 0;
}

static void sync_kill(void)
{
    static const WCHAR *l[] = { L"repllog.exe", NULL };
    scan_procs(l, NULL, TRUE);
}

static void sync_start(void)
{
    PROCESS_INFORMATION pi;
    WCHAR a[24];
    lstrcpy(a, g_as_args);
    if (CreateProcess(L"\\Windows\\repllog.exe", a[0] ? a : NULL, NULL, NULL, FALSE, 0,
                      NULL, NULL, NULL, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        log_line(L"sync: started repllog.exe");
    } else {
        log_line(L"sync: could not start repllog.exe");
    }
}

static BOOL key_down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) ? TRUE : FALSE;
}

/* the key poller only runs when it is needed: learning, or while another program is open */
static void update_key_timer(void)
{
    static BOOL on = FALSE;
    BOOL want = (g_mode == M_KEYS) || (g_child_active && g_homekey);
    if (want && !on) {
        SetTimer(g_wnd, ID_KEY, 40, NULL);
        on = TRUE;
    } else if (!want && on) {
        KillTimer(g_wnd, ID_KEY);
        on = FALSE;
    }
}

static void go_home(void)
{
    g_home_down = FALSE;
    kill_apps();
    if (g_home)
        ShowWindow(g_home, SW_HIDE);
    g_child_active = FALSE;
    g_child_name[0] = 0;
    update_key_timer();
    g_rekill = 2;                               /* a couple of extra sweeps in case something restarts */
    if (g_mode == M_VIDEO)
        set_mode(M_MENU);
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    redraw();
}

static void show_home(const WCHAR *path)
{
    int x, y;
    lstrcpyn(g_child_name, base_name(path), 40);
    g_child_active = TRUE;
    g_rekill = 0;
    g_child_tick = GetTickCount();
    g_keyprev = g_homekey ? key_down(g_homekey) : FALSE;
    update_key_timer();
    if (!g_home || !g_homebtn_on)
        return;
    if (g_home_custom) {
        x = g_home_x;
        y = g_home_y;
    } else {
        switch (g_homepos) {
        case 0:  x = 4;                          y = 4; break;
        case 1:  x = SCR_W - HOME_W - 4;         y = 4; break;
        case 2:  x = 4;                          y = SCR_H - HOME_H - 4; break;
        case 3:  x = SCR_W - HOME_W - 4;         y = SCR_H - HOME_H - 4; break;
        case 4:  x = (SCR_W - HOME_W) / 2;       y = 2; break;
        default: x = (SCR_W - HOME_W) / 2;       y = SCR_H - HOME_H - 2; break;
        }
    }
    g_home_x = x;
    g_home_y = y;
    SetWindowPos(g_home, HWND_TOPMOST, x, y, HOME_W, HOME_H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_home, NULL, FALSE);
}

static void draw_home_btn(HDC dc)
{
    COLORREF c1 = g_ctop[0], c2 = g_cbot[0], edge;
    POINT m[3];
    int y, cx = HOME_W / 2;

    if (g_home_down) {
        c1 = lerp(c1, RGB(255, 255, 255), 30, 100);
        c2 = lerp(c2, RGB(255, 255, 255), 30, 100);
    }
    for (y = 0; y < HOME_H; y++)
        box(dc, 0, y, HOME_W, y + 1, lerp(c1, c2, y, HOME_H - 1));
    edge = lerp(c1, RGB(255, 255, 255), 60, 100);
    box(dc, 0, 0, HOME_W, 1, edge);
    box(dc, 0, HOME_H - 1, HOME_W, HOME_H, edge);
    box(dc, 0, 0, 1, HOME_H, edge);
    box(dc, HOME_W - 1, 0, HOME_W, HOME_H, edge);
    m[0].x = cx - 13; m[0].y = 15;
    m[1].x = cx;      m[1].y = 5;
    m[2].x = cx + 13; m[2].y = 15;
    poly(dc, m, 3, RGB(255, 255, 255));
    box(dc, cx - 9, 15, cx + 9, 25, RGB(255, 255, 255));
    box(dc, cx - 2, 19, cx + 3, 25, c2);
}

static LRESULT CALLBACK HomeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT pt;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        draw_home_btn(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        ClientToScreen(hwnd, &pt);
        g_home_down = TRUE;
        g_dragging = FALSE;
        g_drag_sx = pt.x;
        g_drag_sy = pt.y;
        g_drag_ox = g_home_x;
        g_drag_oy = g_home_y;
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (g_home_down) {
            pt.x = (short)LOWORD(lp);
            pt.y = (short)HIWORD(lp);
            ClientToScreen(hwnd, &pt);
            if (!g_dragging && (iabs(pt.x - g_drag_sx) > 8 || iabs(pt.y - g_drag_sy) > 8))
                g_dragging = TRUE;
            if (g_dragging) {
                int nx = g_drag_ox + (pt.x - g_drag_sx);
                int ny = g_drag_oy + (pt.y - g_drag_sy);
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx > SCR_W - HOME_W) nx = SCR_W - HOME_W;
                if (ny > SCR_H - HOME_H) ny = SCR_H - HOME_H;
                g_home_x = nx;
                g_home_y = ny;
                SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (g_home_down) {
            g_home_down = FALSE;
            if (g_dragging) {                       /* it was moved, not tapped: remember the spot */
                g_dragging = FALSE;
                g_home_custom = TRUE;
                save_homepos();
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                go_home();
            }
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ---- running things ---- */
static void run_app(WCHAR *path, WCHAR *args)
{
    PROCESS_INFORMATION pi;
    if (CreateProcess(path, (args && args[0]) ? args : NULL,
                      NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        show_home(path);
    } else {
        MessageBox(g_wnd, L"Could not start that program.", L"Launcher", MB_OK);
    }
}

static void toggle_taskbar(void)
{
    HWND tb = FindWindow(L"HHTaskBar", NULL);
    if (!tb) {
        MessageBox(g_wnd, L"Taskbar window not found.", L"Launcher", MB_OK);
        return;
    }
    if (IsWindowVisible(tb)) {
        ShowWindow(tb, SW_HIDE);
    } else {
        ShowWindow(tb, SW_SHOW);
        EnableWindow(tb, TRUE);
    }
}

static void go_desktop(void)
{
    HWND tb = FindWindow(L"HHTaskBar", NULL);
    if (tb) {
        ShowWindow(tb, SW_SHOW);
        EnableWindow(tb, TRUE);
    }
    DestroyWindow(g_wnd);
}

/* ---- photo frame ---- */
static void free_image(void)
{
    if (g_imgdc) {
        SelectObject(g_imgdc, g_imgold);
        DeleteDC(g_imgdc);
        g_imgdc = NULL;
    }
    if (g_img) {
        DeleteObject(g_img);
        g_img = NULL;
    }
    g_cbits = NULL;
    g_cfull = FALSE;
}

static BOOL load_image(const WCHAR *path)
{
    HANDLE h;
    BYTE hdr[54];
    DWORD got = 0, off, stride, size, comp;
    int w, ah, bpp;
    LONG hgt;
    BITMAPINFO bi;
    void *bits = NULL;
    HDC sdc;

    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                   FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!ReadFile(h, hdr, 54, &got, NULL) || got != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        CloseHandle(h);
        return FALSE;
    }
    off = rd32(hdr + 10);
    w = (int)rd32(hdr + 18);
    hgt = (LONG)rd32(hdr + 22);
    bpp = (int)rd16(hdr + 28);
    comp = rd32(hdr + 30);
    ah = (hgt < 0) ? (int)(-hgt) : (int)hgt;
    if (rd32(hdr + 14) != 40 || rd16(hdr + 26) != 1 || comp != 0 ||
        (bpp != 16 && bpp != 24 && bpp != 32) ||
        w < 1 || w > 1280 || ah < 1 || ah > 1024) {
        CloseHandle(h);
        return FALSE;
    }
    stride = (((DWORD)w * (DWORD)bpp + 31) / 32) * 4;
    size = stride * (DWORD)ah;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = hgt;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = (WORD)bpp;
    bi.bmiHeader.biCompression = BI_RGB;

    sdc = GetDC(g_wnd);
    g_img = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!g_img || !bits) {
        ReleaseDC(g_wnd, sdc);
        CloseHandle(h);
        g_img = NULL;
        return FALSE;
    }
    if (SetFilePointer(h, (LONG)off, NULL, FILE_BEGIN) == 0xFFFFFFFF ||
        !ReadFile(h, bits, size, &got, NULL) || got != size) {
        ReleaseDC(g_wnd, sdc);
        CloseHandle(h);
        DeleteObject(g_img);
        g_img = NULL;
        return FALSE;
    }
    CloseHandle(h);
    g_imgdc = CreateCompatibleDC(sdc);
    g_imgold = SelectObject(g_imgdc, g_img);
    ReleaseDC(g_wnd, sdc);
    g_iw = w;
    g_ih = ah;
    return TRUE;
}

/* ---- PC companion: persistent canvas that picture patches are copied into ---- */
static BOOL make_canvas(void)
{
    BITMAPINFO bi;
    HDC sdc;
    void *bits = NULL;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = SCR_W;
    bi.bmiHeader.biHeight = SCR_H;              /* bottom-up */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 16;
    bi.bmiHeader.biCompression = BI_RGB;
    sdc = GetDC(g_wnd);
    g_img = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!g_img || !bits) {
        ReleaseDC(g_wnd, sdc);
        g_img = NULL;
        return FALSE;
    }
    g_imgdc = CreateCompatibleDC(sdc);
    g_imgold = SelectObject(g_imgdc, g_img);
    ReleaseDC(g_wnd, sdc);
    g_cbits = (BYTE *)bits;
    memset(g_cbits, 0, SCR_W * SCR_H * 2);
    g_iw = SCR_W;
    g_ih = SCR_H;
    g_cfull = FALSE;
    return TRUE;
}

/* A patch is a 16-bit BMP whose "pixels per metre" header fields carry the x,y position
   where it goes on the screen. A full picture is just a 320x240 patch at 0,0. */
static BOOL apply_patch(const WCHAR *path)
{
    HANDLE h;
    BYTE hdr[54];
    DWORD got = 0, off, size;
    int w, ph, x, y, j;

    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                   FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!ReadFile(h, hdr, 54, &got, NULL) || got != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        CloseHandle(h);
        return FALSE;
    }
    off = rd32(hdr + 10);
    w = (int)rd32(hdr + 18);
    ph = (int)rd32(hdr + 22);
    x = (int)rd32(hdr + 38);
    y = (int)rd32(hdr + 42);
    if (rd32(hdr + 14) != 40 || rd16(hdr + 28) != 16 || rd32(hdr + 30) != 0 ||
        w < 2 || (w & 1) || ph < 1 || x < 0 || y < 0 || x + w > SCR_W || y + ph > SCR_H) {
        CloseHandle(h);
        return FALSE;
    }
    size = (DWORD)w * 2 * (DWORD)ph;
    if (SetFilePointer(h, (LONG)off, NULL, FILE_BEGIN) == 0xFFFFFFFF ||
        !ReadFile(h, g_patchbuf, size, &got, NULL) || got != size) {
        CloseHandle(h);
        return FALSE;
    }
    CloseHandle(h);
    if (!g_img && !make_canvas())
        return FALSE;
    for (j = 0; j < ph; j++)
        memcpy(g_cbits + (size_t)(SCR_H - y - ph + j) * SCR_W * 2 + (size_t)x * 2,
               g_patchbuf + (size_t)j * w * 2, (size_t)w * 2);
    if (w == SCR_W && ph == SCR_H)
        g_cfull = TRUE;
    return TRUE;
}

/* asks the PC script for a complete picture (it checks for this file) */
static void request_keyframe(void)
{
    HANDLE h = CreateFile(L"\\Temp\\c710need.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

static void scan_frames(void)
{
    static const WCHAR *dirs[] = { L"\\Storage Card\\Frame", L"\\My Flash Disk\\Frame", NULL };
    int d;
    WIN32_FIND_DATA fd;
    HANDLE h;
    WCHAR pat[100];

    g_nfiles = 0;
    g_cur = 0;
    g_dir[0] = 0;
    for (d = 0; dirs[d] && g_nfiles == 0; d++) {
        lstrcpy(g_dir, dirs[d]);
        wsprintf(pat, L"%s\\*.bmp", dirs[d]);
        h = FindFirstFile(pat, &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                g_nfiles < MAX_FILES && lstrlen(fd.cFileName) < 72)
                lstrcpy(g_files[g_nfiles++], fd.cFileName);
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
}

/* dir: +1 next, -1 previous, 0 show current */
static void show_frame(int dir)
{
    int step = (dir < 0) ? -1 : 1;
    int tries;

    free_image();
    if (g_nfiles == 0)
        return;
    if (dir != 0)
        g_cur = (g_cur + step + g_nfiles) % g_nfiles;
    for (tries = 0; tries < g_nfiles; tries++) {
        WCHAR path[160];
        wsprintf(path, L"%s\\%s", g_dir, g_files[g_cur]);
        if (load_image(path))
            return;
        g_cur = (g_cur + step + g_nfiles) % g_nfiles;
    }
}

/* ---- videos ---- */
static BOOL reg_default(const WCHAR *subkey, WCHAR *out, DWORD cch)
{
    HKEY k;
    DWORD type = 0, cb = cch * sizeof(WCHAR);
    LONG r;
    if (RegOpenKeyEx(HKEY_CLASSES_ROOT, subkey, 0, 0, &k) != ERROR_SUCCESS)
        return FALSE;
    out[0] = 0;
    r = RegQueryValueEx(k, NULL, NULL, &type, (LPBYTE)out, &cb);
    RegCloseKey(k);
    return (r == ERROR_SUCCESS && type == REG_SZ && out[0]) ? TRUE : FALSE;
}

/* split a registered "open" command into program + arguments, filling in %1 */
static void parse_cmd(const WCHAR *cmd, const WCHAR *file, WCHAR *exe, WCHAR *args)
{
    const WCHAR *q = cmd;
    int n = 0, hasarg = 0;
    while (*q == L' ')
        q++;
    if (*q == L'"') {
        q++;
        while (*q && *q != L'"' && n < 119)
            exe[n++] = *q++;
        if (*q == L'"')
            q++;
    } else {
        while (*q && *q != L' ' && n < 119)
            exe[n++] = *q++;
    }
    exe[n] = 0;
    while (*q == L' ')
        q++;
    n = 0;
    while (*q && n < 180) {
        if (q[0] == L'%' && q[1] == L'1') {
            const WCHAR *f = file;
            BOOL quoted_before = (n > 0 && args[n - 1] == L'"');
            if (!quoted_before)
                args[n++] = L'"';
            while (*f && n < 190)
                args[n++] = *f++;
            if (q[2] != L'"')
                args[n++] = L'"';
            q += 2;
            hasarg = 1;
        } else {
            args[n++] = *q++;
        }
    }
    args[n] = 0;
    if (!hasarg) {
        const WCHAR *f = file;
        if (n > 0)
            args[n++] = L' ';
        args[n++] = L'"';
        while (*f && n < 195)
            args[n++] = *f++;
        args[n++] = L'"';
        args[n] = 0;
    }
}

static BOOL is_video_ext(const WCHAR *n)
{
    static const WCHAR *ex[] = { L".avi", L".wmv", L".asf", L".mpg", L".mpeg", L".mp4",
                                 L".3gp", L".m4v", L".mov", L".divx", L".xvid", NULL };
    int len = lstrlen(n), i;
    for (i = 0; ex[i]; i++) {
        int el = lstrlen(ex[i]);
        if (len > el && !lstrcmpi(n + len - el, ex[i]))
            return TRUE;
    }
    return FALSE;
}

static void scan_dir(const WCHAR *dir)
{
    WIN32_FIND_DATA fd;
    HANDLE h;
    WCHAR pat[120];

    wsprintf(pat, L"%s\\*.*", dir);
    h = FindFirstFile(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && is_video_ext(fd.cFileName) &&
            g_nvid < MAX_VID && lstrlen(dir) + lstrlen(fd.cFileName) < 96) {
            VidFile *v = &g_vid[g_nvid++];
            WCHAR *dot, *c;
            lstrcpyn(v->name, fd.cFileName, 44);
            wsprintf(v->path, L"%s\\%s", dir, fd.cFileName);
            v->mb = fd.nFileSizeLow / 1048576;
            dot = NULL;
            for (c = v->name; *c; c++) {
                if (*c == L'.')
                    dot = c;
            }
            if (dot)
                *dot = 0;
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

static void scan_videos(void)
{
    int i, j;
    g_nvid = 0;
    g_vtop = 0;
    g_vpress = -1;
    scan_dir(L"\\Storage Card");
    scan_dir(L"\\Storage Card\\Video");
    scan_dir(L"\\Storage Card\\Videos");
    scan_dir(L"\\Storage Card\\Movies");
    scan_dir(L"\\My Flash Disk");
    scan_dir(L"\\My Flash Disk\\Video");
    for (i = 1; i < g_nvid; i++) {           /* simple alphabetical sort */
        for (j = i; j > 0 && lstrcmpi(g_vid[j - 1].name, g_vid[j].name) > 0; j--) {
            VidFile t = g_vid[j];
            g_vid[j] = g_vid[j - 1];
            g_vid[j - 1] = t;
        }
    }
}

static void launch_video(int idx)
{
    WCHAR cmd[220], ext[16], progid[80], key[120], exe[130], args[220];
    const WCHAR *p = g_vid[idx].path, *c, *dot = NULL;
    BOOL found = FALSE;

    exe[0] = 0;
    args[0] = 0;
    if (g_videoapp[0]) {
        lstrcpy(exe, g_videoapp);
        wsprintf(args, L"\"%s\"", p);
        found = TRUE;
    } else {
        for (c = p; *c; c++) {
            if (*c == L'.')
                dot = c;
        }
        if (dot && lstrlen(dot) < 12) {
            lstrcpy(ext, dot);
            if (reg_default(ext, progid, 80)) {
                wsprintf(key, L"%s\\Shell\\Open\\Command", progid);
                if (reg_default(key, cmd, 220)) {
                    parse_cmd(cmd, p, exe, args);
                    found = TRUE;
                }
            }
            if (!found) {
                wsprintf(key, L"%s\\Shell\\Open\\Command", ext);
                if (reg_default(key, cmd, 220)) {
                    parse_cmd(cmd, p, exe, args);
                    found = TRUE;
                }
            }
        }
    }
    if (!found) {                              /* last resort: Mio's own player */
        lstrcpy(exe, L"\\Windows\\MP3Player.exe");
        wsprintf(args, L"\"%s\"", p);
    }
    wsprintf(cmd, L"video: %s  %s  (registry handler %s)", exe, args, found ? L"yes" : L"no");
    log_line(cmd);
    run_app(exe, args);
}

static int video_row_at(int x, int y)
{
    int rel = y - (BAR_H + 4), i;
    if (rel < 0 || x < 8 || x >= SCR_W - 8)
        return -1;
    i = rel / 34;
    if (i >= VROWS || rel % 34 >= 32)
        return -1;
    i += g_vtop;
    return (i < g_nvid) ? i : -1;
}

static void video_scroll(int d)
{
    int maxtop = g_nvid - VROWS;
    g_vtop += d;
    if (g_vtop > maxtop)
        g_vtop = maxtop;
    if (g_vtop < 0)
        g_vtop = 0;
}

static void draw_video(HDC dc)
{
    int i, y0 = BAR_H + 4;
    WCHAR t[24];

    draw_bg(dc);
    draw_topbar(dc, L"Videos");
    if (g_nvid == 0) {
        icon_video(dc, SCR_W / 2, 74, RGB(150, 60, 112));
        text(dc, f_med, C_TEXT, L"No videos found", 0, 104, SCR_W, 128, DT_CENTER | DT_SINGLELINE);
        text(dc, f_small, C_DIM, L"Put .avi .wmv .asf .mpg files on the SD card", 0, 134, SCR_W, 152,
             DT_CENTER | DT_SINGLELINE);
    }
    for (i = 0; i < VROWS; i++) {
        int idx = g_vtop + i, y = y0 + i * 34;
        COLORREF c1 = g_ctop[5], c2 = g_cbot[5];
        if (idx >= g_nvid)
            break;
        if (idx == g_vpress) {
            c1 = lerp(c1, RGB(255, 255, 255), 30, 100);
            c2 = lerp(c2, RGB(255, 255, 255), 30, 100);
        }
        grad_rrect(dc, 8, y, SCR_W - 8, y + 32, c1, c2);
        box(dc, 15, y + 1, SCR_W - 15, y + 2, lerp(c1, RGB(255, 255, 255), 45, 100));
        icon_video(dc, 28, y + 16, c2);
        text(dc, f_label, C_TEXT, g_vid[idx].name, 52, y, SCR_W - 70, y + 32,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        wsprintf(t, L"%u MB", g_vid[idx].mb);
        text(dc, f_small, C_TEXT, t, SCR_W - 70, y, SCR_W - 14, y + 32,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    /* footer: Back / Up / Down */
    grad_rrect(dc, 8, 214, 96, 238, g_ctop[4], g_cbot[4]);
    shadowed(dc, f_label, C_TEXT, L"Back", 8, 214, 96, 238, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    grad_rrect(dc, 104, 214, 204, 238, g_ctop[0], g_cbot[0]);
    seg(dc, 144, 229, 154, 221, C_TEXT, 3);
    seg(dc, 154, 221, 164, 229, C_TEXT, 3);
    grad_rrect(dc, 212, 214, 312, 238, g_ctop[0], g_cbot[0]);
    seg(dc, 252, 221, 262, 229, C_TEXT, 3);
    seg(dc, 262, 229, 272, 221, C_TEXT, 3);
}

/* ---- hardware button learn / test screen ---- */
static void khist_add(const WCHAR *t)
{
    int i;
    for (i = 6; i > 0; i--)
        lstrcpy(g_khist[i], g_khist[i - 1]);
    lstrcpyn(g_khist[0], t, 40);
    if (g_nkh < 7)
        g_nkh++;
}

static void key_msg(int vk, int sc)
{
    WCHAR b[40];
    wsprintf(b, L"message: 0x%02X (%d)  scan 0x%02X", vk, vk, sc);
    khist_add(b);
    if (g_learning && vk > 7 && vk < 255) {
        g_homekey = vk;
        g_learning = FALSE;
        save_homekey();
        toast(L"Home key saved");
    }
    redraw();
}

static void on_key_timer(void)
{
    int vk;
    if (g_mode == M_KEYS) {
        BOOL changed = FALSE;
        for (vk = 8; vk < 255; vk++) {
            BYTE d = key_down(vk) ? 1 : 0;
            if (d && !g_kbase[vk]) {
                WCHAR b[40];
                wsprintf(b, L"poll: 0x%02X (%d)", vk, vk);
                khist_add(b);
                if (g_learning) {
                    g_homekey = vk;
                    g_learning = FALSE;
                    save_homekey();
                    toast(L"Home key saved");
                }
                changed = TRUE;
            }
            g_kbase[vk] = d;
        }
        if (changed)
            redraw();
    }
    if (g_child_active && g_homekey) {
        BOOL d = key_down(g_homekey);
        if (d && !g_keyprev)
            go_home();
        g_keyprev = d;
    }
}

static void draw_keys(HDC dc)
{
    int i;
    WCHAR t[60];

    draw_bg(dc);
    draw_topbar(dc, L"Hardware buttons");
    text(dc, f_small, C_DIM, L"Press any button on the device to see its code.", 10, 34, SCR_W - 10, 50,
         DT_LEFT | DT_SINGLELINE);
    if (g_homekey)
        wsprintf(t, L"Home key: 0x%02X (%d)", g_homekey, g_homekey);
    else
        lstrcpy(t, L"Home key: not set");
    text(dc, f_med, C_ACCENT, t, 10, 52, SCR_W - 10, 76, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (i = 0; i < g_nkh && i < 6; i++)
        text(dc, f_small, C_TEXT, g_khist[i], 10, 82 + i * 17, SCR_W - 10, 99 + i * 17,
             DT_LEFT | DT_SINGLELINE);
    if (g_learning) {
        rrect(dc, 20, 190, 300, 210, 8, RGB(40, 12, 34), C_ACCENT);
        text(dc, f_small, C_ACCENT, L"Now press the button you want for Home", 20, 190, 300, 210,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    grad_rrect(dc, 8, 214, 96, 238, g_ctop[4], g_cbot[4]);
    shadowed(dc, f_label, C_TEXT, L"Back", 8, 214, 96, 238, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    grad_rrect(dc, 104, 214, 204, 238, g_ctop[0], g_cbot[0]);
    shadowed(dc, f_label, C_TEXT, L"Learn", 104, 214, 204, 238, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    grad_rrect(dc, 212, 214, 312, 238, g_ctop[4], g_cbot[4]);
    shadowed(dc, f_label, C_TEXT, L"Clear", 212, 214, 312, 238, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void draw_toast(HDC dc)
{
    rrect(dc, 20, 4, SCR_W - 20, 28, 10, RGB(40, 12, 34), C_ACCENT);
    text(dc, f_small, C_TEXT, g_toast, 20, 4, SCR_W - 20, 28, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* ---- modes ---- */
static BOOL keeps_awake(int m) { return m == M_FRAME || m == M_CLOCK || m == M_PC; }

static void set_mode(int m)
{
    if (keeps_awake(g_mode) && !keeps_awake(m))
        signal_evt(L"EVENT_APP_ENABLE_SUSPEND");
    if (!keeps_awake(g_mode) && keeps_awake(m))
        signal_evt(L"EVENT_APP_DISABLE_SUSPEND");
    if ((g_mode == M_FRAME || g_mode == M_PC) && m != g_mode)
        free_image();
    if (g_mode == M_PC && m != M_PC)
        KillTimer(g_wnd, ID_POLL);
    if (g_mode == M_PC && m != M_PC)
        log_line(L"pc: screen mode closed");
    if (m == M_KEYS && g_mode != M_KEYS) {
        int vk;
        for (vk = 0; vk < 256; vk++)
            g_kbase[vk] = (vk >= 8 && vk < 255 && key_down(vk)) ? 1 : 0;
        g_nkh = 0;
        g_learning = FALSE;
        SetFocus(g_wnd);
    }
    if (g_mode == M_KEYS && m != M_KEYS)
        g_learning = FALSE;
    if (m == M_PC && g_mode != M_PC) {
        log_line(L"pc: screen mode started");
        if (!sync_running())
            sync_start();                       /* make sure the USB link program is up */
        g_lastframe = GetTickCount();
        g_lastrestart = 0;
        g_sync_up = FALSE;
        g_gotframe = FALSE;
        g_nosig = FALSE;
        DeleteFile(PC_FRAME);               /* drop any stale patch */
        request_keyframe();
        SetTimer(g_wnd, ID_POLL, 100, NULL);
    }
    g_mode = m;
    update_key_timer();
    g_secs = 0;
    g_pressed = -1;
    if (m == M_FRAME) {
        scan_frames();
        show_frame(0);
    }
    if (m == M_VIDEO)
        scan_videos();
    redraw();
}

static int dim(int m, int y)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12)
        return 31;
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return d[m - 1];
}

static void begin_settime(void)
{
    SYSTEMTIME st;
    int i;
    GetLocalTime(&st);
    g_f[0] = st.wDay;
    g_f[1] = st.wMonth;
    g_f[2] = st.wYear;
    g_f[3] = st.wHour;
    g_f[4] = st.wMinute;
    for (i = 0; i < 5; i++) {
        if (g_f[i] < g_fmin[i]) g_f[i] = g_fmin[i];
        if (g_f[i] > g_fmax[i]) g_f[i] = g_fmax[i];
    }
    set_mode(M_SETTIME);
}

static void st_step(int i, int dir)
{
    int v = g_f[i] + dir, d;
    if (v > g_fmax[i]) v = g_fmin[i];
    if (v < g_fmin[i]) v = g_fmax[i];
    g_f[i] = v;
    d = dim(g_f[1], g_f[2]);
    if (g_f[0] > d)
        g_f[0] = d;
}

static BOOL st_button(int x, int y, int *field, int *dir)
{
    int i;
    for (i = 0; i < 5; i++) {
        int x0 = 20 + i * 56;
        if (x >= x0 && x < x0 + 52) {
            if (y >= 44 && y < 94)   { *field = i; *dir = 1;  return TRUE; }
            if (y >= 146 && y < 196) { *field = i; *dir = -1; return TRUE; }
        }
    }
    return FALSE;
}

static void st_save(void)
{
    SYSTEMTIME st;
    int d = dim(g_f[1], g_f[2]);
    GetLocalTime(&st);
    st.wDay = (WORD)(g_f[0] > d ? d : g_f[0]);
    st.wMonth = (WORD)g_f[1];
    st.wYear = (WORD)g_f[2];
    st.wHour = (WORD)g_f[3];
    st.wMinute = (WORD)g_f[4];
    st.wSecond = 0;
    st.wMilliseconds = 0;
    SetLocalTime(&st);
}

static void do_tile(int idx)
{
    Tile *t = &g_tiles[idx];
    switch (t->action) {
    case A_FRAME:   set_mode(M_FRAME); break;
    case A_CLOCK:   set_mode(M_CLOCK); break;
    case A_SETTIME: begin_settime(); break;
    case A_TASKBAR: toggle_taskbar(); break;
    case A_DESKTOP: go_desktop(); break;
    case A_PC:      set_mode(M_PC); break;
    case A_VIDEO:   set_mode(M_VIDEO); break;
    case A_KEYS:    set_mode(M_KEYS); break;
    case A_SYNC:
        lstrcpyn(g_as_args, t->args, 24);       /* remember which flavour you use */
        if (sync_running()) {
            sync_kill();                        /* fresh start for a stuck connection */
            Sleep(600);
        }
        sync_start();
        toast(L"ActiveSync started");
        break;
    case A_RUN:     run_app(t->path, t->args); break;
    }
}

/* ---- drawing ---- */
static void make_background(void)
{
    int y;
    for (y = 0; y < SCR_H; y++)
        box(g_bgdc, 0, y, SCR_W, y + 1, lerp(BG_TOP, BG_BOT, y, SCR_H - 1));
}

static void draw_bg(HDC dc)
{
    BitBlt(dc, 0, 0, SCR_W, SCR_H, g_bgdc, 0, 0, SRCCOPY);
}

static void draw_topbar(HDC dc, const WCHAR *title)
{
    SYSTEMTIME st;
    WCHAR t[48];
    int y;

    /* darkened strip that fades into the background */
    for (y = 0; y < BAR_H; y++)
        box(dc, 0, y, SCR_W, y + 1,
            lerp(RGB(34, 10, 28), lerp(BG_TOP, BG_BOT, y, SCR_H - 1), y, BAR_H));
    box(dc, 0, BAR_H - 1, SCR_W, BAR_H, lerp(C_ACCENT, BG_TOP, 55, 100));

    ell(dc, 9, 11, 17, 19, C_ACCENT, NONE);
    text(dc, f_med, C_TEXT, title, 24, 0, 190, BAR_H - 1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    GetLocalTime(&st);
    wsprintf(t, L"%s %u %s  %02u:%02u", g_days3[st.wDayOfWeek % 7], st.wDay,
             g_months3[(st.wMonth + 11) % 12], st.wHour, st.wMinute);
    text(dc, f_small, C_TEXT, t, 150, 0, SCR_W - 8, BAR_H - 1,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void draw_tile(HDC dc, int idx, int slot)
{
    RECT rc;
    const Tile *t = &g_tiles[idx];
    BOOL down = (idx == g_pressed);
    COLORREF c1 = g_ctop[t->color], c2 = g_cbot[t->color];
    int off = down ? 1 : 0;
    int cx;

    tile_rect(slot, &rc);
    cx = (rc.left + rc.right) / 2;

    if (down) {
        c1 = lerp(c1, RGB(255, 255, 255), 28, 100);
        c2 = lerp(c2, RGB(255, 255, 255), 28, 100);
    } else {
        /* soft shadow */
        grad_rrect(dc, rc.left + 2, rc.top + 3, rc.right + 2, rc.bottom + 3,
                   RGB(40, 12, 34), RGB(52, 16, 44));
    }
    grad_rrect(dc, rc.left + off, rc.top + off, rc.right + off, rc.bottom + off, c1, c2);
    /* gloss line along the top edge */
    box(dc, rc.left + off + 7, rc.top + off + 1, rc.right + off - 7, rc.top + off + 2,
        lerp(c1, RGB(255, 255, 255), 45, 100));

    draw_icon(dc, t, cx + off, rc.top + off + 33);
    shadowed(dc, f_label, C_TEXT, t->name, rc.left + 2 + off, rc.top + 56 + off,
             rc.right - 2 + off, rc.bottom - 2 + off,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void draw_footer(HDC dc)
{
    int pages = page_count(), p, x0;
    COLORREF dim_c = RGB(150, 92, 130);

    if (pages < 2)
        return;
    x0 = SCR_W / 2 - (pages * 14) / 2 + 4;
    for (p = 0; p < pages; p++) {
        int x = x0 + p * 14;
        if (p == g_page)
            ell(dc, x - 1, SCR_H - 14, x + 9, SCR_H - 4, C_ACCENT, NONE);
        else
            ell(dc, x + 1, SCR_H - 12, x + 7, SCR_H - 6, dim_c, NONE);
    }
    /* chevrons */
    seg(dc, 34, SCR_H - 17, 26, SCR_H - 9, C_TEXT, 2);
    seg(dc, 26, SCR_H - 9, 34, SCR_H - 1, C_TEXT, 2);
    seg(dc, SCR_W - 34, SCR_H - 17, SCR_W - 26, SCR_H - 9, C_TEXT, 2);
    seg(dc, SCR_W - 26, SCR_H - 9, SCR_W - 34, SCR_H - 1, C_TEXT, 2);
}

static void draw_menu(HDC dc)
{
    int slot, first = g_page * PER_PAGE;

    draw_bg(dc);
    draw_topbar(dc, L"Mio C710");
    for (slot = 0; slot < PER_PAGE; slot++) {
        int idx = first + slot;
        if (idx >= g_ntiles)
            break;
        draw_tile(dc, idx, slot);
    }
    draw_footer(dc);
}

static void draw_clock(HDC dc)
{
    SYSTEMTIME st;
    WCHAR t[64];
    int w;

    GetLocalTime(&st);
    draw_bg(dc);

    wsprintf(t, L"%02u:%02u", st.wHour, st.wMinute);
    shadowed(dc, f_big, C_TEXT, t, 0, 22, SCR_W, 138, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* accent divider */
    box(dc, SCR_W / 2 - 40, 144, SCR_W / 2 + 40, 146, C_ACCENT);

    wsprintf(t, L"%s", g_days[st.wDayOfWeek % 7]);
    text(dc, f_date, C_ACCENT, t, 0, 152, SCR_W, 178, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    wsprintf(t, L"%u %s %u", st.wDay, g_months[(st.wMonth + 11) % 12], st.wYear);
    text(dc, f_date, C_DIM, t, 0, 178, SCR_W, 204, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* seconds bar */
    box(dc, 30, 222, SCR_W - 30, 227, RGB(84, 36, 70));
    w = ((SCR_W - 60) * (int)st.wSecond) / 59;
    if (w > 0)
        box(dc, 30, 222, 30 + w, 227, C_ACCENT);
}

/* box blur helpers for the soft shadow */
static void blur_h(const BYTE *src, BYTE *dst, int w, int h, int r)
{
    int x, y, n = 2 * r + 1;
    for (y = 0; y < h; y++) {
        const BYTE *sr = src + y * w;
        BYTE *dr = dst + y * w;
        int sum = 0;
        for (x = -r; x <= r; x++)
            sum += sr[x < 0 ? 0 : (x >= w ? w - 1 : x)];
        for (x = 0; x < w; x++) {
            int xo = x - r, xi = x + r + 1;
            dr[x] = (BYTE)(sum / n);
            sum -= sr[xo < 0 ? 0 : xo];
            sum += sr[xi >= w ? w - 1 : xi];
        }
    }
}

static void blur_v(const BYTE *src, BYTE *dst, int w, int h, int r)
{
    int x, y, n = 2 * r + 1;
    for (x = 0; x < w; x++) {
        int sum = 0;
        for (y = -r; y <= r; y++)
            sum += src[(y < 0 ? 0 : (y >= h ? h - 1 : y)) * w + x];
        for (y = 0; y < h; y++) {
            int yo = y - r, yi = y + r + 1;
            dst[y * w + x] = (BYTE)(sum / n);
            sum -= src[(yo < 0 ? 0 : yo) * w + x];
            sum += src[(yi >= h ? h - 1 : yi) * w + x];
        }
    }
}

static HBITMAP make_dib16(HDC like, int w, int h, HDC *pdc, HGDIOBJ *pold, BYTE **pbits)
{
    BITMAPINFO bi;
    void *bits = NULL;
    HBITMAP bm;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 16;
    bi.bmiHeader.biCompression = BI_RGB;
    bm = CreateDIBSection(like, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits)
        return NULL;
    *pdc = CreateCompatibleDC(like);
    *pold = SelectObject(*pdc, bm);
    *pbits = (BYTE *)bits;
    return bm;
}

#define SC_W 240
#define SC_H 104

/* big clock + date with a soft, diffused shadow under the letters (bottom-right corner) */
static void soft_clock(HDC dc)
{
    static BYTE a1[SC_W * SC_H], a2[SC_W * SC_H];
    SYSTEMTIME st;
    WCHAR tm[8], dt[40];
    HDC tdc = NULL, mdc = NULL;
    HBITMAP tbm, mbm;
    HGDIOBJ told = NULL, mold = NULL;
    BYTE *tb = NULL, *mb = NULL;
    int rx = SCR_W - SC_W - 2, ry = SCR_H - SC_H - 2, i;

    GetLocalTime(&st);
    wsprintf(tm, L"%02u:%02u", st.wHour, st.wMinute);
    wsprintf(dt, L"%s, %u %s", g_days[st.wDayOfWeek % 7], st.wDay, g_months3[(st.wMonth + 11) % 12]);

    tbm = make_dib16(dc, SC_W, SC_H, &tdc, &told, &tb);
    mbm = make_dib16(dc, SC_W, SC_H, &mdc, &mold, &mb);
    if (tbm && mbm) {
        const WORD *mp = (const WORD *)mb;
        WORD *tp = (WORD *)tb;

        /* 1. the text drawn white-on-black into a mask, slightly offset down-right */
        BitBlt(tdc, 0, 0, SC_W, SC_H, dc, rx, ry, SRCCOPY);
        box(mdc, 0, 0, SC_W, SC_H, RGB(0, 0, 0));
        text(mdc, f_clk, RGB(255, 255, 255), tm, 2, 6, SC_W - 6, 68, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        text(mdc, f_date, RGB(255, 255, 255), dt, 2, 70, SC_W - 8, 98, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        for (i = 0; i < SC_W * SC_H; i++)
            a1[i] = (BYTE)((mp[i] & 31) * 8);

        /* 2. blur the mask so it becomes a soft glow */
        blur_h(a1, a2, SC_W, SC_H, 6);
        blur_v(a2, a1, SC_W, SC_H, 6);
        blur_h(a1, a2, SC_W, SC_H, 5);
        blur_v(a2, a1, SC_W, SC_H, 5);

        /* 3. darken the photo underneath by the blurred amount */
        for (i = 0; i < SC_W * SC_H; i++) {
            int d = (a1[i] * 5) >> 1;              /* boost, the blur makes it faint */
            WORD p = tp[i];
            int r, g, b;
            if (d > 150)
                d = 150;
            r = (((p >> 10) & 31) * (256 - d)) >> 8;
            g = (((p >> 5) & 31) * (256 - d)) >> 8;
            b = ((p & 31) * (256 - d)) >> 8;
            tp[i] = (WORD)((r << 10) | (g << 5) | b);
        }
        BitBlt(dc, rx, ry, SC_W, SC_H, tdc, 0, 0, SRCCOPY);
    }
    if (mdc) {
        SelectObject(mdc, mold);
        DeleteDC(mdc);
    }
    if (tdc) {
        SelectObject(tdc, told);
        DeleteDC(tdc);
    }
    if (mbm)
        DeleteObject(mbm);
    if (tbm)
        DeleteObject(tbm);

    /* crisp text on top */
    text(dc, f_clk, RGB(255, 255, 255), tm, rx, ry + 4, rx + SC_W - 8, ry + 66, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    text(dc, f_date, RGB(255, 214, 230), dt, rx, ry + 68, rx + SC_W - 10, ry + 96, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

static void draw_frame(HDC dc)
{
    if (g_img && g_imgdc) {
        box(dc, 0, 0, SCR_W, SCR_H, RGB(0, 0, 0));
        if (g_iw == SCR_W && g_ih == SCR_H) {
            BitBlt(dc, 0, 0, SCR_W, SCR_H, g_imgdc, 0, 0, SRCCOPY);
        } else {
            int dw, dh;
            if (g_iw * SCR_H > g_ih * SCR_W) {
                dw = SCR_W;
                dh = g_ih * SCR_W / g_iw;
            } else {
                dh = SCR_H;
                dw = g_iw * SCR_H / g_ih;
            }
            StretchBlt(dc, (SCR_W - dw) / 2, (SCR_H - dh) / 2, dw, dh,
                       g_imgdc, 0, 0, g_iw, g_ih, SRCCOPY);
        }
    } else {
        draw_bg(dc);
        icon_photo(dc, SCR_W / 2, 70, RGB(150, 60, 112));
        text(dc, f_med, C_TEXT, L"No pictures found", 0, 100, SCR_W, 124, DT_CENTER | DT_SINGLELINE);
        text(dc, f_small, C_DIM, L"Put 320x240 .bmp files in", 0, 130, SCR_W, 148, DT_CENTER | DT_SINGLELINE);
        text(dc, f_small, C_ACCENT, L"\\Storage Card\\Frame", 0, 148, SCR_W, 166, DT_CENTER | DT_SINGLELINE);
        text(dc, f_small, C_DIM, L"Tap the top-left corner to go back", 0, 190, SCR_W, 208, DT_CENTER | DT_SINGLELINE);
    }
    if (g_showclock)
        soft_clock(dc);
}

/* draw the current g_img full screen (scaled to fit if it is not 320x240) */
static void blit_image(HDC dc)
{
    if (g_iw == SCR_W && g_ih == SCR_H) {
        BitBlt(dc, 0, 0, SCR_W, SCR_H, g_imgdc, 0, 0, SRCCOPY);
    } else {
        int dw, dh;
        if (g_iw * SCR_H > g_ih * SCR_W) {
            dw = SCR_W;
            dh = g_ih * SCR_W / g_iw;
        } else {
            dh = SCR_H;
            dw = g_iw * SCR_H / g_ih;
        }
        StretchBlt(dc, (SCR_W - dw) / 2, (SCR_H - dh) / 2, dw, dh,
                   g_imgdc, 0, 0, g_iw, g_ih, SRCCOPY);
    }
}

static void draw_pc(HDC dc)
{
    if (g_gotframe && g_img && g_imgdc) {
        box(dc, 0, 0, SCR_W, SCR_H, RGB(0, 0, 0));
        blit_image(dc);
        if (g_nosig) {
            rrect(dc, SCR_W - 104, 6, SCR_W - 6, 30, 10, RGB(0, 0, 0), NONE);
            text(dc, f_small, C_ACCENT, L"no signal", SCR_W - 104, 6, SCR_W - 6, 30,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else {
        draw_bg(dc);
        icon_monitor(dc, SCR_W / 2, 66, RGB(150, 60, 112), RGB(255, 190, 215));
        text(dc, f_med, C_TEXT, L"Waiting for your PC...", 0, 98, SCR_W, 122, DT_CENTER | DT_SINGLELINE);
        text(dc, f_small, C_DIM, L"Plug in the USB cable, then run", 0, 128, SCR_W, 146, DT_CENTER | DT_SINGLELINE);
        text(dc, f_small, C_ACCENT, L"c710_display.py on your PC", 0, 146, SCR_W, 164, DT_CENTER | DT_SINGLELINE);
        text(dc, f_small, C_DIM, L"Tap the top-left corner to go back", 0, 190, SCR_W, 208, DT_CENTER | DT_SINGLELINE);
    }
}

static void pm_button(HDC dc, int x0, int y0, int plus, BOOL tall)
{
    int cx = x0 + 26, cy = y0 + 25;
    COLORREF w = RGB(255, 255, 255);
    (void)tall;
    grad_rrect(dc, x0, y0, x0 + 52, y0 + 50, g_ctop[0], g_cbot[0]);
    seg(dc, cx - 9, cy, cx + 9, cy, w, 3);
    if (plus)
        seg(dc, cx, cy - 9, cx, cy + 9, w, 3);
}

static void draw_settime(HDC dc)
{
    static const WCHAR *lab[5] = { L"Day", L"Month", L"Year", L"Hour", L"Min" };
    int i;
    WCHAR t[16];

    draw_bg(dc);
    draw_topbar(dc, L"Set date and time");
    for (i = 0; i < 5; i++) {
        int x0 = 20 + i * 56;
        text(dc, f_small, C_DIM, lab[i], x0, 30, x0 + 52, 44, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        pm_button(dc, x0, 44, 1, TRUE);
        rrect(dc, x0, 98, x0 + 52, 142, 6, RGB(46, 16, 40), RGB(190, 110, 158));
        if (i == 2)
            wsprintf(t, L"%04d", g_f[i]);
        else
            wsprintf(t, L"%02d", g_f[i]);
        text(dc, f_date, C_TEXT, t, x0, 98, x0 + 52, 142, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        pm_button(dc, x0, 146, 0, TRUE);
    }
    grad_rrect(dc, 20, 202, 150, 234, g_ctop[0], g_cbot[0]);
    shadowed(dc, f_med, C_TEXT, L"Save", 20, 202, 150, 234, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    grad_rrect(dc, 170, 202, 300, 234, g_ctop[4], g_cbot[4]);
    shadowed(dc, f_med, C_TEXT, L"Cancel", 170, 202, 300, 234, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void render(HDC dc)
{
    switch (g_mode) {
    case M_CLOCK:   draw_clock(dc); break;
    case M_FRAME:   draw_frame(dc); break;
    case M_SETTIME: draw_settime(dc); break;
    case M_PC:      draw_pc(dc); break;
    case M_VIDEO:   draw_video(dc); break;
    case M_KEYS:    draw_keys(dc); break;
    default:        draw_menu(dc); break;
    }
    if (g_toast[0] && GetTickCount() < g_toast_until)
        draw_toast(dc);
}

/* ---- input ---- */
static void on_down(int x, int y)
{
    int field, dir;
    g_downx = x;
    g_downy = y;
    if (g_mode == M_MENU) {
        g_pressed = tile_at(x, y);
        redraw();
    } else if (g_mode == M_VIDEO) {
        g_vpress = video_row_at(x, y);
        redraw();
    } else if (g_mode == M_SETTIME) {
        if (st_button(x, y, &field, &dir)) {
            g_rep_field = field;
            g_rep_dir = dir;
            st_step(field, dir);
            SetTimer(g_wnd, ID_REPEAT, 150, NULL);
            redraw();
        }
    }
}

static void on_up(int x, int y)
{
    int dx = x - g_downx, dy = y - g_downy;

    if (g_mode == M_MENU) {
        int t = tile_at(x, y);
        int hit = g_pressed;
        g_pressed = -1;
        if (iabs(dx) >= 50 && iabs(dx) > 2 * iabs(dy)) {
            change_page(dx < 0 ? 1 : -1);
        } else if (t >= 0 && t == hit) {
            do_tile(t);
        } else if (y >= SCR_H - 30 && page_count() > 1) {
            if (x < 100)
                change_page(-1);
            else if (x >= 220)
                change_page(1);
        }
        redraw();
    } else if (g_mode == M_CLOCK) {
        set_mode(M_MENU);
    } else if (g_mode == M_PC) {
        if (x < 60 && y < 60)
            set_mode(M_MENU);
    } else if (g_mode == M_FRAME) {
        if (x < 60 && y < 60) {
            set_mode(M_MENU);
        } else if (iabs(dx) >= 50 && iabs(dx) > 2 * iabs(dy)) {
            g_secs = 0;
            show_frame(dx < 0 ? 1 : -1);
            redraw();
        } else if (x < SCR_W / 3) {
            g_secs = 0;
            show_frame(-1);
            redraw();
        } else if (x >= 2 * SCR_W / 3) {
            g_secs = 0;
            show_frame(1);
            redraw();
        } else {
            g_showclock = !g_showclock;
            redraw();
        }
    } else if (g_mode == M_KEYS) {
        if (y >= 212) {
            if (x < 100) {
                set_mode(M_MENU);
            } else if (x < 208) {
                g_learning = TRUE;
                khist_add(L"waiting for a button...");
            } else {
                g_homekey = 0;
                g_learning = FALSE;
                DeleteFile(L"\\Storage Card\\launcher_key.txt");
                toast(L"Home key cleared");
            }
        }
        redraw();
    } else if (g_mode == M_VIDEO) {
        int row = video_row_at(x, y), hit = g_vpress;
        g_vpress = -1;
        if (iabs(dy) >= 40 && iabs(dy) > 2 * iabs(dx)) {
            video_scroll(dy < 0 ? VROWS : -VROWS);
        } else if (row >= 0 && row == hit) {
            launch_video(row);
        } else if (y >= 212) {
            if (x < 100)
                set_mode(M_MENU);
            else if (x < 208)
                video_scroll(-VROWS);
            else
                video_scroll(VROWS);
        }
        redraw();
    } else if (g_mode == M_SETTIME) {
        KillTimer(g_wnd, ID_REPEAT);
        if (y >= 202 && y < 234) {
            if (x >= 20 && x < 150) {
                st_save();
                set_mode(M_MENU);
            } else if (x >= 170 && x < 300) {
                set_mode(M_MENU);
            }
        }
    }
}

static void on_timer(UINT id)
{
    if (id == ID_REPEAT) {
        st_step(g_rep_field, g_rep_dir);
        redraw();
    } else if (id == ID_POLL) {
        if (GetFileAttributes(PC_FRAME) != 0xFFFFFFFF) {
            if (apply_patch(PC_FRAME)) {
                if (g_cfull)
                    g_gotframe = TRUE;
                if (g_nosig)
                    log_line(L"pc: pictures resumed");
                g_nosig = FALSE;
                g_lastframe = GetTickCount();
            }
            DeleteFile(PC_FRAME);           /* tells the PC it can send the next one */
            redraw();
        } else if (g_gotframe && !g_nosig && GetTickCount() - g_lastframe > 12000) {
            g_nosig = TRUE;
            log_line(L"pc: no pictures for 12 s");
            redraw();
        }
    } else if (id == ID_KEY) {
        on_key_timer();
    } else if (id == ID_TICK) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        g_tickn++;
        if (g_toast[0] && GetTickCount() >= g_toast_until) {
            g_toast[0] = 0;
            redraw();
        }
        if (keeps_awake(g_mode) && g_tickn % 20 == 0)
            signal_evt(L"EVENT_APP_DISABLE_SUSPEND");     /* say it again now and then */
        if (g_child_active) {
            if (GetForegroundWindow() == g_wnd && GetTickCount() - g_child_tick > 6000) {
                if (g_home)
                    ShowWindow(g_home, SW_HIDE);          /* back on the launcher by itself */
                g_child_active = FALSE;
                update_key_timer();
            } else if (g_home && g_homebtn_on) {
                SetWindowPos(g_home, HWND_TOPMOST, g_home_x, g_home_y, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        } else if (g_rekill > 0) {
            g_rekill--;
            kill_apps();
            SetForegroundWindow(g_wnd);
        }
        /* keep the USB link to the PC alive while the PC screen is showing */
        if (g_mode == M_PC && g_tickn % 3 == 0) {
            BOOL up = sync_running();
            if (up != g_sync_up) {
                g_sync_up = up;
                log_line(up ? L"pc: repllog.exe is running" : L"pc: repllog.exe is NOT running");
            }
            if (!up) {
                sync_start();
            } else if (g_gotframe && g_nosig && GetTickCount() - g_lastframe > 25000 &&
                       GetTickCount() - g_lastrestart > 25000) {
                log_line(L"pc: no pictures for 25 s, restarting ActiveSync");
                sync_kill();
                Sleep(600);
                sync_start();
                g_lastrestart = GetTickCount();
            }
        }
        if (g_mode == M_FRAME) {
            g_secs++;
            if (g_secs >= FRAME_SECS) {
                g_secs = 0;
                show_frame(1);
                redraw();
            }
        }
        if (g_mode == M_CLOCK) {
            redraw();                       /* seconds bar moves every second */
        } else if (g_mode != M_SETTIME && (int)st.wMinute != g_lastmin) {
            redraw();                       /* only when the minute changes */
        }
        g_lastmin = (int)st.wMinute;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_back) {
            render(g_back);
            BitBlt(dc, 0, 0, SCR_W, SCR_H, g_back, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        on_down((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_LBUTTONUP:
        on_up((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_mode == M_KEYS)
            key_msg((int)wp, (int)((lp >> 16) & 0xFF));
        return 0;
    case WM_TIMER:
        on_timer((UINT)wp);
        return 0;
    case WM_DESTROY:
        if (keeps_awake(g_mode))
            signal_evt(L"EVENT_APP_ENABLE_SUSPEND");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    WNDCLASS wc;
    MSG msg;
    HDC sdc;
    HANDLE mtx;

    mtx = CreateMutex(NULL, TRUE, L"C710AutoLauncher");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    f_small  = mkfont(13, 0);
    f_label  = mkfont(15, 1);
    f_med    = mkfont(17, 1);
    f_big    = mkfont(88, 1);
    f_date   = mkfont(21, 0);
    f_over   = mkfont(30, 1);
    f_letter = mkfont(28, 1);
    f_clk    = mkfont(58, 1);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"C710Launcher";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClass(&wc);

    g_wnd = CreateWindow(L"C710Launcher", L"Launcher", WS_POPUP | WS_VISIBLE,
                         0, 0, SCR_W, SCR_H, NULL, NULL, hInst, NULL);
    if (!g_wnd)
        return 1;

    sdc = GetDC(g_wnd);
    g_back = CreateCompatibleDC(sdc);
    g_backbmp = CreateCompatibleBitmap(sdc, SCR_W, SCR_H);
    g_backold = SelectObject(g_back, g_backbmp);
    g_bgdc = CreateCompatibleDC(sdc);
    g_bgbmp = CreateCompatibleBitmap(sdc, SCR_W, SCR_H);
    g_bgold = SelectObject(g_bgdc, g_bgbmp);
    ReleaseDC(g_wnd, sdc);

    make_background();
    build_tiles();
    load_saved();

    {
        WNDCLASS hc;
        memset(&hc, 0, sizeof(hc));
        hc.lpfnWndProc = HomeProc;
        hc.hInstance = hInst;
        hc.lpszClassName = L"C710Home";
        hc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClass(&hc);
        g_home = CreateWindowEx(WS_EX_TOPMOST, L"C710Home", L"", WS_POPUP, 0, 0, HOME_W, HOME_H,
                                NULL, NULL, hInst, NULL);
    }

    ShowWindow(g_wnd, SW_SHOW);
    UpdateWindow(g_wnd);
    SetForegroundWindow(g_wnd);
    SetTimer(g_wnd, ID_TICK, 1000, NULL);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    free_image();
    SelectObject(g_bgdc, g_bgold);
    DeleteObject(g_bgbmp);
    DeleteDC(g_bgdc);
    SelectObject(g_back, g_backold);
    DeleteObject(g_backbmp);
    DeleteDC(g_back);
    return (int)msg.wParam;
}

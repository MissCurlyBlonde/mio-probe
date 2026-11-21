/*
 * C710auto.c  (v3, pink edition)
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
#define NONE       ((COLORREF)0xFFFFFFFF)

/* palette */
#define BG_TOP     RGB(62, 22, 52)
#define BG_BOT     RGB(128, 52, 104)
#define C_TEXT     RGB(255, 248, 252)
#define C_DIM      RGB(228, 184, 208)
#define C_ACCENT   RGB(255, 196, 214)

enum { M_MENU, M_CLOCK, M_FRAME, M_SETTIME };
enum { A_FRAME, A_CLOCK, A_SETTIME, A_TASKBAR, A_DESKTOP, A_RUN };
enum { I_PHOTO, I_CLOCK, I_CAL, I_FOLDER, I_BAR, I_HOME, I_GRID, I_MUSIC, I_APP };

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
static HFONT f_small, f_label, f_med, f_big, f_date, f_over, f_letter;

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
    default:       icon_app(dc, cx, cy, dk, t->name); break;
    }
}

static void redraw(void)
{
    if (g_wnd)
        InvalidateRect(g_wnd, NULL, FALSE);
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
    add_tile(L"Clock",    NULL, NULL, A_CLOCK,   I_CLOCK,  1);
    add_tile(L"Set time", NULL, NULL, A_SETTIME, I_CAL,    2);
    add_tile(L"Files",    L"\\Windows\\explorer.exe", L"\\Storage Card", A_RUN, I_FOLDER, 7);
    add_tile(L"Taskbar",  NULL, NULL, A_TASKBAR, I_BAR,    4);
    add_tile(L"Desktop",  NULL, NULL, A_DESKTOP, I_HOME,   3);
    add_tile(L"Mio menu", L"\\Windows\\MainShell.exe", NULL, A_RUN, I_GRID,  0);
    add_tile(L"Mio MP3",  L"\\Windows\\MP3Player.exe", NULL, A_RUN, I_MUSIC, 6);
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

/* ---- running things ---- */
static void run_app(WCHAR *path, WCHAR *args)
{
    PROCESS_INFORMATION pi;
    if (CreateProcess(path, (args && args[0]) ? args : NULL,
                      NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
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

/* ---- modes ---- */
static BOOL keeps_awake(int m) { return m == M_FRAME || m == M_CLOCK; }

static void set_mode(int m)
{
    if (keeps_awake(g_mode) && !keeps_awake(m))
        signal_evt(L"EVENT_APP_ENABLE_SUSPEND");
    if (!keeps_awake(g_mode) && keeps_awake(m))
        signal_evt(L"EVENT_APP_DISABLE_SUSPEND");
    if (g_mode == M_FRAME && m != M_FRAME)
        free_image();
    g_mode = m;
    g_secs = 0;
    g_pressed = -1;
    if (m == M_FRAME) {
        scan_frames();
        show_frame(0);
    }
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
             rc.right - 2 + off, rc.bottom - 2 + off, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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

static void draw_frame(HDC dc)
{
    SYSTEMTIME st;
    WCHAR t[16];

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
    if (g_showclock) {
        GetLocalTime(&st);
        wsprintf(t, L"%02u:%02u", st.wHour, st.wMinute);
        rrect(dc, SCR_W - 98, SCR_H - 44, SCR_W - 8, SCR_H - 8, 10, RGB(0, 0, 0), NONE);
        text(dc, f_over, C_TEXT, t, SCR_W - 98, SCR_H - 44, SCR_W - 8, SCR_H - 8,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
    default:        draw_menu(dc); break;
    }
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
    } else if (id == ID_TICK) {
        SYSTEMTIME st;
        GetLocalTime(&st);
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

/*
 * C710auto.c - touch launcher, photo frame and clock for the Mio DigiWalker C710
 * Windows CE .NET 4.2, ARM, 320x240, plain GDI (no DirectDraw, no image decoders).
 *
 * Put the built C710auto.exe in the ROOT of the SD card. Mio's ST.exe starts it at boot
 * (and hides the taskbar, so this program is the only thing on screen).
 *
 * Photo frame:  BMP files (uncompressed 16/24/32-bit) in \Storage Card\Frame
 *               (fallback: \My Flash Disk\Frame). 320x240 is ideal; other sizes are
 *               scaled to fit. See make_frames.py for a PC-side converter.
 * Extra tiles:  optional text file \Storage Card\launcher.txt, one tile per line:
 *                   Name|\path\to\program.exe|optional arguments
 *               lines starting with # are ignored.
 *
 * Not compiled on a real toolchain by the author - report compile errors and they get fixed.
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
#define BAR_H      26
#define GAP        6
#define COLS       3
#define ROWS       2
#define PER_PAGE   (COLS * ROWS)
#define MAX_TILES  40
#define MAX_FILES  400
#define FRAME_SECS 10
#define ID_TICK    1
#define ID_REPEAT  2

#define C_BG      RGB(18, 22, 30)
#define C_BAR     RGB(34, 42, 58)
#define C_TILE    RGB(48, 72, 120)
#define C_TILE_DN RGB(95, 135, 205)
#define C_TEXT    RGB(240, 240, 240)
#define C_DIM     RGB(150, 160, 180)

enum { M_MENU, M_CLOCK, M_FRAME, M_SETTIME };
enum { A_FRAME, A_CLOCK, A_SETTIME, A_TASKBAR, A_DESKTOP, A_RUN };

typedef struct {
    WCHAR name[28];
    WCHAR path[100];
    WCHAR args[100];
    int action;
} Tile;

/* ---- state ---- */
static Tile g_tiles[MAX_TILES];
static int g_ntiles = 0;
static int g_page = 0;
static int g_mode = M_MENU;
static int g_pressed = -1;
static HWND g_wnd = NULL;

static HDC g_back = NULL;
static HBITMAP g_backbmp = NULL;
static HGDIOBJ g_backold = NULL;
static HFONT f_small, f_med, f_big, f_date, f_over;

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
static const WCHAR *g_months[12] = {
    L"January", L"February", L"March", L"April", L"May", L"June",
    L"July", L"August", L"September", L"October", L"November", L"December"
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

static void box(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc;
    HBRUSH br = CreateSolidBrush(c);
    rc.left = l; rc.top = t; rc.right = r; rc.bottom = b;
    FillRect(dc, &rc, br);
    DeleteObject(br);
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

static void outlined(HDC dc, HFONT f, const WCHAR *s,
                     int l, int t, int r, int b, UINT fmt)
{
    int dx, dy;
    for (dx = -2; dx <= 2; dx += 2)
        for (dy = -2; dy <= 2; dy += 2)
            if (dx || dy)
                text(dc, f, RGB(0, 0, 0), s, l + dx, t + dy, r + dx, b + dy, fmt);
    text(dc, f, RGB(255, 255, 255), s, l, t, r, b, fmt);
}

static void redraw(void)
{
    if (g_wnd)
        InvalidateRect(g_wnd, NULL, FALSE);
}

/* ---- tiles ---- */
static void add_tile(const WCHAR *name, const WCHAR *path, const WCHAR *args, int action)
{
    Tile *t;
    if (g_ntiles >= MAX_TILES)
        return;
    t = &g_tiles[g_ntiles++];
    lstrcpyn(t->name, name, 28);
    lstrcpyn(t->path, path ? path : L"", 100);
    lstrcpyn(t->args, args ? args : L"", 100);
    t->action = action;
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
        add_tile(line, p1 + 1, p2 ? p2 + 1 : NULL, A_RUN);
    }
}

static void build_tiles(void)
{
    add_tile(L"Photos",   NULL, NULL, A_FRAME);
    add_tile(L"Clock",    NULL, NULL, A_CLOCK);
    add_tile(L"Set time", NULL, NULL, A_SETTIME);
    add_tile(L"Files",    L"\\Windows\\explorer.exe", L"\\Storage Card", A_RUN);
    add_tile(L"Taskbar",  NULL, NULL, A_TASKBAR);
    add_tile(L"Desktop",  NULL, NULL, A_DESKTOP);
    add_tile(L"Mio menu", L"\\Windows\\MainShell.exe", NULL, A_RUN);
    add_tile(L"Mio MP3",  L"\\Windows\\MP3Player.exe", NULL, A_RUN);
    load_config();
}

static void tile_rect(int slot, RECT *rc)
{
    int col = slot % COLS, row = slot / COLS;
    int w = (SCR_W - GAP * (COLS + 1)) / COLS;
    int h = (SCR_H - 2 * BAR_H - GAP * (ROWS + 1)) / ROWS;
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
static void draw_menu(HDC dc)
{
    int slot, first = g_page * PER_PAGE, pages = page_count();
    SYSTEMTIME st;
    WCHAR t[40];

    box(dc, 0, 0, SCR_W, SCR_H, C_BG);
    box(dc, 0, 0, SCR_W, BAR_H, C_BAR);
    GetLocalTime(&st);
    text(dc, f_med, C_TEXT, L"Launcher", 8, 0, 200, BAR_H, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    wsprintf(t, L"%02u:%02u", st.wHour, st.wMinute);
    text(dc, f_med, C_TEXT, t, SCR_W - 90, 0, SCR_W - 8, BAR_H, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    for (slot = 0; slot < PER_PAGE; slot++) {
        RECT rc;
        int idx = first + slot;
        if (idx >= g_ntiles)
            break;
        tile_rect(slot, &rc);
        box(dc, rc.left, rc.top, rc.right, rc.bottom, (idx == g_pressed) ? C_TILE_DN : C_TILE);
        text(dc, f_med, C_TEXT, g_tiles[idx].name, rc.left + 2, rc.top, rc.right - 2, rc.bottom,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (pages > 1) {
        wsprintf(t, L"%d / %d", g_page + 1, pages);
        text(dc, f_small, C_DIM, t, 100, SCR_H - BAR_H, 220, SCR_H, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, f_big == NULL ? f_med : f_med, C_TEXT, L"<", 0, SCR_H - BAR_H, 100, SCR_H,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, f_med, C_TEXT, L">", 220, SCR_H - BAR_H, SCR_W, SCR_H,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void draw_clock(HDC dc)
{
    SYSTEMTIME st;
    WCHAR t[64];

    GetLocalTime(&st);
    box(dc, 0, 0, SCR_W, SCR_H, RGB(0, 0, 0));
    wsprintf(t, L"%02u:%02u", st.wHour, st.wMinute);
    text(dc, f_big, RGB(255, 255, 255), t, 0, 40, SCR_W, 150, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    wsprintf(t, L"%s %u %s %u", g_days[st.wDayOfWeek % 7], st.wDay,
             g_months[(st.wMonth + 11) % 12], st.wYear);
    text(dc, f_date, C_DIM, t, 0, 150, SCR_W, 190, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void draw_frame(HDC dc)
{
    SYSTEMTIME st;
    WCHAR t[16];

    box(dc, 0, 0, SCR_W, SCR_H, RGB(0, 0, 0));
    if (g_img && g_imgdc) {
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
        text(dc, f_med, C_TEXT, L"No pictures found", 0, 70, SCR_W, 100, DT_CENTER | DT_SINGLELINE);
        text(dc, f_small, C_DIM, L"Put 320x240 .bmp files in", 0, 105, SCR_W, 125, DT_CENTER | DT_SINGLELINE);
        text(dc, f_small, C_DIM, L"\\Storage Card\\Frame", 0, 125, SCR_W, 145, DT_CENTER | DT_SINGLELINE);
        text(dc, f_small, C_DIM, L"Tap the top-left corner to go back", 0, 170, SCR_W, 190, DT_CENTER | DT_SINGLELINE);
    }
    if (g_showclock) {
        GetLocalTime(&st);
        wsprintf(t, L"%02u:%02u", st.wHour, st.wMinute);
        outlined(dc, f_over, t, SCR_W - 140, SCR_H - 52, SCR_W - 8, SCR_H - 6,
                 DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
    }
}

static void draw_settime(HDC dc)
{
    static const WCHAR *lab[5] = { L"Day", L"Month", L"Year", L"Hour", L"Min" };
    int i;
    WCHAR t[16];

    box(dc, 0, 0, SCR_W, SCR_H, C_BG);
    box(dc, 0, 0, SCR_W, BAR_H, C_BAR);
    text(dc, f_med, C_TEXT, L"Set date and time", 8, 0, SCR_W, BAR_H, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (i = 0; i < 5; i++) {
        int x0 = 20 + i * 56;
        text(dc, f_small, C_DIM, lab[i], x0, 28, x0 + 52, 44, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        box(dc, x0, 44, x0 + 52, 94, C_TILE);
        text(dc, f_date, C_TEXT, L"+", x0, 44, x0 + 52, 94, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (i == 2)
            wsprintf(t, L"%04d", g_f[i]);
        else
            wsprintf(t, L"%02d", g_f[i]);
        text(dc, f_date, C_TEXT, t, x0, 94, x0 + 52, 146, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        box(dc, x0, 146, x0 + 52, 196, C_TILE);
        text(dc, f_date, C_TEXT, L"-", x0, 146, x0 + 52, 196, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    box(dc, 20, 202, 150, 234, C_TILE);
    text(dc, f_med, C_TEXT, L"Save", 20, 202, 150, 234, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    box(dc, 170, 202, 300, 234, C_TILE);
    text(dc, f_med, C_TEXT, L"Cancel", 170, 202, 300, 234, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
    if (g_mode == M_MENU) {
        int t = tile_at(x, y);
        int hit = g_pressed;
        g_pressed = -1;
        if (t >= 0 && t == hit) {
            do_tile(t);
        } else if (y >= SCR_H - BAR_H && page_count() > 1) {
            if (x < 100)
                g_page = (g_page + page_count() - 1) % page_count();
            else if (x >= 220)
                g_page = (g_page + 1) % page_count();
        }
        redraw();
    } else if (g_mode == M_CLOCK) {
        set_mode(M_MENU);
    } else if (g_mode == M_FRAME) {
        if (x < 60 && y < 60) {
            set_mode(M_MENU);
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
        if (g_mode == M_FRAME) {
            g_secs++;
            if (g_secs >= FRAME_SECS) {
                g_secs = 0;
                show_frame(1);
            }
        }
        if (g_mode != M_SETTIME)
            redraw();
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

    f_small = mkfont(13, 0);
    f_med   = mkfont(17, 1);
    f_big   = mkfont(88, 1);
    f_date  = mkfont(22, 0);
    f_over  = mkfont(34, 1);

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
    ReleaseDC(g_wnd, sdc);

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
    SelectObject(g_back, g_backold);
    DeleteObject(g_backbmp);
    DeleteDC(g_back);
    return (int)msg.wParam;
}

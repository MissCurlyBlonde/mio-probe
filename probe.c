/*
 * probe.c - system probe for Windows CE .NET 4.2 (ARM)
 * Writes a report to \Storage Card\probe.txt (falls back to \My Documents, then \).
 *
 * Build options:
 *   eVC++ 4.0 SP4: new "WCE Application", target ARMV4 (Release), add this file.
 *   cegcc:         arm-mingw32ce-gcc -o probe.exe probe.c
 *
 * NOT compiled/tested by the author - report any compile errors and they get fixed.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#ifndef SPI_GETPLATFORMTYPE
#define SPI_GETPLATFORMTYPE 257
#endif
#ifndef SPI_GETOEMINFO
#define SPI_GETOEMINFO 258
#endif

static HANDLE g_file = INVALID_HANDLE_VALUE;

static void put(const WCHAR *s)
{
    char buf[512];
    DWORD n, w;
    n = WideCharToMultiByte(CP_ACP, 0, s, -1, buf, sizeof(buf), NULL, NULL);
    if (n > 1 && g_file != INVALID_HANDLE_VALUE)
        WriteFile(g_file, buf, n - 1, &w, NULL);
}

static void line(const WCHAR *s)
{
    put(s);
    put(L"\r\n");
}

static void section(const WCHAR *title)
{
    put(L"\r\n=== ");
    put(title);
    put(L" ===\r\n");
}

static void list_subkeys(const WCHAR *path, int max)
{
    HKEY hk;
    WCHAR name[128], t[220];
    DWORD len, i;

    wsprintf(t, L"[HKLM\\%s]", path);
    line(t);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, 0, &hk) != ERROR_SUCCESS) {
        line(L"  (not present)");
        return;
    }
    for (i = 0; i < (DWORD)max; i++) {
        len = 128;
        if (RegEnumKeyEx(hk, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        wsprintf(t, L"  %s", name);
        line(t);
    }
    RegCloseKey(hk);
}

static void dump_values(const WCHAR *path)
{
    HKEY hk;
    WCHAR name[128], t[400];
    DWORD buf[64];              /* DWORD array keeps ARM alignment safe */
    DWORD i, nl, dl, type;

    wsprintf(t, L"[HKLM\\%s] values", path);
    line(t);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, 0, &hk) != ERROR_SUCCESS) {
        line(L"  (not present)");
        return;
    }
    for (i = 0; i < 200; i++) {
        nl = 128;
        dl = sizeof(buf) - sizeof(WCHAR);
        if (RegEnumValue(hk, i, name, &nl, NULL, &type, (LPBYTE)buf, &dl) != ERROR_SUCCESS)
            break;
        if (type == REG_SZ) {
            ((WCHAR *)buf)[dl / sizeof(WCHAR)] = 0;
            wsprintf(t, L"  %s = %s", name, (WCHAR *)buf);
        } else if (type == REG_DWORD && dl >= 4) {
            wsprintf(t, L"  %s = %lu", name, buf[0]);
        } else {
            wsprintf(t, L"  %s (type %lu, %lu bytes)", name, type, dl);
        }
        line(t);
    }
    RegCloseKey(hk);
}

static void list_dir(const WCHAR *pattern, int max)
{
    WIN32_FIND_DATA fd;
    HANDLE h;
    WCHAR t[400];
    int n = 0;

    wsprintf(t, L"[dir %s]", pattern);
    line(t);
    h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        line(L"  (none)");
        return;
    }
    do {
        wsprintf(t, L"  %s  attr=0x%lX size=%lu", fd.cFileName,
                 fd.dwFileAttributes, fd.nFileSizeLow);
        line(t);
        n++;
    } while (n < max && FindNextFile(h, &fd));
    FindClose(h);
}

static void disk(const WCHAR *path)
{
    ULARGE_INTEGER avail, total, freeb;
    WCHAR t[220];

    if (GetDiskFreeSpaceEx(path, &avail, &total, &freeb))
        wsprintf(t, L"  %s: total %lu MB, free %lu MB", path,
                 (DWORD)(total.QuadPart >> 20), (DWORD)(freeb.QuadPart >> 20));
    else
        wsprintf(t, L"  %s: not available", path);
    line(t);
}

static void check_libs(void)
{
    static const WCHAR *libs[] = {
        L"ddraw.dll",       /* DirectDraw (fast framebuffer access)    */
        L"gx.dll",          /* GAPI (many old ports need it)           */
        L"mscoree.dll",     /* .NET Compact Framework                  */
        L"htmlview.dll",    /* HTML control                            */
        L"imgdecmp.dll",    /* image decompression                     */
        L"imaging.dll",     /* imaging API                             */
        L"quartz.dll",      /* DirectShow                              */
        L"winsock.dll",
        L"ws2.dll",
        L"wininet.dll",
        L"ndis.dll",
        L"afd.dll",         /* sockets driver                          */
        L"btdrt.dll",       /* Bluetooth                               */
        L"wavedev.dll",     /* audio driver                            */
        L"aygshell.dll",    /* PocketPC shell helpers                  */
        NULL
    };
    int i;
    HMODULE h;
    WCHAR t[200];

    for (i = 0; libs[i]; i++) {
        h = LoadLibrary(libs[i]);
        if (h) {
            wsprintf(t, L"  %s: YES", libs[i]);
            FreeLibrary(h);
        } else {
            wsprintf(t, L"  %s: no (err %lu)", libs[i], GetLastError());
        }
        line(t);
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow)
{
    static const WCHAR *paths[] = {
        L"\\Storage Card\\probe.txt",
        L"\\My Documents\\probe.txt",
        L"\\probe.txt",
        NULL
    };
    static const WCHAR *vols[] = {
        L"\\", L"\\Storage Card", L"\\My Flash Disk", L"\\SD Card", L"\\Flash Disk", NULL
    };
    OSVERSIONINFO osvi;
    SYSTEM_INFO si;
    MEMORYSTATUS ms;
    SYSTEMTIME st;
    WCHAR t[400], buf[128];
    const WCHAR *used = NULL;
    HDC hdc;
    int i;

    for (i = 0; paths[i]; i++) {
        g_file = CreateFile(paths[i], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_file != INVALID_HANDLE_VALUE) {
            used = paths[i];
            break;
        }
    }
    if (g_file == INVALID_HANDLE_VALUE) {
        MessageBox(NULL, L"Could not create probe.txt anywhere", L"probe", MB_OK);
        return 1;
    }

    line(L"probe report");

    /* ---- OS ---- */
    section(L"OS");
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (GetVersionEx(&osvi)) {
        wsprintf(t, L"  version %lu.%lu build %lu platformId %lu csd '%s'",
                 osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber,
                 osvi.dwPlatformId, osvi.szCSDVersion);
        line(t);
    }
    buf[0] = 0;
    if (SystemParametersInfo(SPI_GETPLATFORMTYPE, sizeof(buf), buf, 0)) {
        wsprintf(t, L"  platform type: %s", buf);
        line(t);
    }
    buf[0] = 0;
    if (SystemParametersInfo(SPI_GETOEMINFO, sizeof(buf), buf, 0)) {
        wsprintf(t, L"  OEM info: %s", buf);
        line(t);
    }
    GetLocalTime(&st);
    wsprintf(t, L"  device clock: %04u-%02u-%02u %02u:%02u:%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    line(t);

    /* ---- CPU / memory ---- */
    section(L"CPU and memory");
    GetSystemInfo(&si);
    wsprintf(t, L"  arch %u, type %lu, level %u, rev %u, page size %lu",
             (unsigned)si.wProcessorArchitecture, si.dwProcessorType,
             (unsigned)si.wProcessorLevel, (unsigned)si.wProcessorRevision,
             si.dwPageSize);
    line(t);
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    wsprintf(t, L"  physical: total %lu KB, free %lu KB", ms.dwTotalPhys / 1024,
             ms.dwAvailPhys / 1024);
    line(t);
    wsprintf(t, L"  virtual: total %lu KB, free %lu KB, load %lu%%",
             ms.dwTotalVirtual / 1024, ms.dwAvailVirtual / 1024, ms.dwMemoryLoad);
    line(t);

    /* ---- Display ---- */
    section(L"Display");
    wsprintf(t, L"  screen %d x %d", GetSystemMetrics(SM_CXSCREEN),
             GetSystemMetrics(SM_CYSCREEN));
    line(t);
    hdc = GetDC(NULL);
    if (hdc) {
        wsprintf(t, L"  bits per pixel %d, dpi %d x %d",
                 GetDeviceCaps(hdc, BITSPIXEL), GetDeviceCaps(hdc, LOGPIXELSX),
                 GetDeviceCaps(hdc, LOGPIXELSY));
        line(t);
        ReleaseDC(NULL, hdc);
    }

    /* ---- Storage ---- */
    section(L"Storage");
    for (i = 0; vols[i]; i++)
        disk(vols[i]);
    list_dir(L"\\*", 100);
    list_dir(L"\\Storage Card\\*", 100);
    list_dir(L"\\My Flash Disk\\*", 100);

    /* ---- Registry ---- */
    section(L"Registry: startup");
    dump_values(L"Init");
    section(L"Registry: drivers");
    list_subkeys(L"Drivers\\BuiltIn", 200);
    list_subkeys(L"Drivers\\USB\\FunctionDrivers", 50);
    list_subkeys(L"Drivers\\USB\\ClientDrivers", 50);
    section(L"Registry: networking");
    list_subkeys(L"Comm", 100);

    /* ---- Libraries ---- */
    section(L"Libraries");
    check_libs();

    /* ---- Windows directory ---- */
    section(L"\\Windows contents");
    list_dir(L"\\Windows\\*.exe", 400);
    list_dir(L"\\Windows\\*.dll", 600);

    line(L"\r\nend of report");
    CloseHandle(g_file);

    wsprintf(t, L"Report written to:\n%s", used);
    MessageBox(NULL, t, L"probe", MB_OK);
    return 0;
}

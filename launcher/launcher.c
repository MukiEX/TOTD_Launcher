/*
 * TotD2P launcher
 *
 * - Shows every keyboard you press a key on as a card in a Player 1 or
 *   Player 2 column. Press Left/A or Right/D *on a keyboard* to move that
 *   keyboard's card. Enter starts the game; Esc quits.
 * - Owns Raw Input (background sink) and publishes per-player key state to
 *   the proxy dinput.dll through shared memory, so it must stay open while
 *   you play. Assignments are remembered in tod2p.ini (by device path).
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include "../common/tod2p_shm.h"

#ifndef RIDEV_DEVNOTIFY
#define RIDEV_DEVNOTIFY 0x00002000
#endif
#ifndef WM_INPUT_DEVICE_CHANGE
#define WM_INPUT_DEVICE_CHANGE 0x00FE
#endif
#ifndef GIDC_ARRIVAL
#define GIDC_ARRIVAL 1
#define GIDC_REMOVAL 2
#endif

/* hid.dll (declared by hand to avoid DDK header differences) */
__declspec(dllimport) BOOLEAN __stdcall HidD_GetProductString(HANDLE, PVOID, ULONG);
__declspec(dllimport) BOOLEAN __stdcall HidD_GetManufacturerString(HANDLE, PVOID, ULONG);

#define MAX_KBD      32
#define TIMER_ID     1
#define TIMER_MS     100

typedef struct {
    HANDLE   h;
    WCHAR    path[512];
    WCHAR    name[160];
    WCHAR    detail[96];
    int      player;        /* 1 or 2 */
    int      connected;
    int      shown;         /* has produced input (or is a remembered P2) */
    int      order;
    DWORD    lastActivity;
    uint8_t  keys[256];     /* DIK held on this device */
} Kbd;

static Kbd          g_kbd[MAX_KBD];
static int          g_nkbd, g_orderCounter;
static HWND         g_hwnd;
static HANDLE       g_map;
static Tod2pShared *g_shm;
static uint8_t      g_cnt[3][256];        /* [player][dik] devices holding */
static WCHAR        g_iniPath[MAX_PATH];
static WCHAR        g_gameExe[MAX_PATH];
static WCHAR        g_status[256];
static HANDLE       g_gameProc;
static HFONT        g_fTitle, g_fName, g_fSmall, g_fBig;
static uint32_t     g_display;            /* TOD2P_DISPLAY_* */

/* ------------------------------------------------------------------------ */
static void set_status(const WCHAR *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(g_status, 255, fmt, ap);
    g_status[255] = 0;
    va_end(ap);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void display_name(uint32_t mode, WCHAR *out, int n)
{
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int bw = sh * 4 / 3, bh = sh;
    if (bw > sw) { bw = sw; bh = sw * 3 / 4; }
    switch (mode) {
    case TOD2P_DISPLAY_FULL_NATIVE:
        _snwprintf(out, n, L"Fullscreen %dx%d (stretched)", sw, sh); break;
    case TOD2P_DISPLAY_BORDERLESS:
        _snwprintf(out, n, L"Borderless 4:3 %dx%d", bw & ~1, bh & ~1); break;
    case TOD2P_DISPLAY_WINDOW2X:
        _snwprintf(out, n, L"Window 1280x960"); break;
    default:
        _snwprintf(out, n, L"Original 640x480"); break;
    }
    out[n - 1] = 0;
}

static void set_display(uint32_t mode)
{
    g_display = mode % TOD2P_DISPLAY_COUNT;
    g_shm->displayMode = g_display;
    WCHAR v[4]; _snwprintf(v, 3, L"%u", g_display); v[3] = 0;
    WritePrivateProfileStringW(L"Display", L"Mode", v, g_iniPath);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void exe_dir(WCHAR *out, const WCHAR *exe)
{
    lstrcpynW(out, exe, MAX_PATH);
    WCHAR *s = wcsrchr(out, L'\\');
    if (s) s[1] = 0; else out[0] = 0;
}

static int file_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ---- shared memory publishing ------------------------------------------- */
static void push_p1_event(uint8_t dik, int down, DWORD t)
{
    uint32_t w = g_shm->p1Write;
    Tod2pKeyEvent *e = &g_shm->p1ring[w & (TOD2P_RING - 1)];
    e->dik = dik; e->down = (uint8_t)down; e->pad = 0; e->time = t;
    MemoryBarrier();
    g_shm->p1Write = w + 1;
}

static void player_key(int player, uint8_t dik, int down, DWORD t)
{
    uint8_t *c = &g_cnt[player][dik];
    int was = *c > 0;
    if (down) { if (*c < 255) (*c)++; } else if (*c) (*c)--;
    int now = *c > 0;
    if (was == now) return;
    if (player == 1) {
        g_shm->p1state[dik] = now ? 0x80 : 0;
        push_p1_event(dik, now, t);
    } else {
        g_shm->p2state[dik] = now ? 0x80 : 0;
    }
}

static void device_key(Kbd *k, uint8_t dik, int down, DWORD t)
{
    if (down) {
        if (k->keys[dik]) return;                /* autorepeat */
        k->keys[dik] = 1;
    } else {
        if (!k->keys[dik]) return;
        k->keys[dik] = 0;
    }
    player_key(k->player, dik, down, t);
}

static void release_all(Kbd *k)
{
    DWORD t = GetTickCount();
    for (int d = 0; d < 256; d++)
        if (k->keys[d]) device_key(k, (uint8_t)d, 0, t);
}

/* ---- device list -------------------------------------------------------- */
static void describe(Kbd *k)
{
    WCHAR prod[128] = L"", mfr[128] = L"";
    HANDLE f = CreateFileW(k->path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (!HidD_GetProductString(f, prod, sizeof prod)) prod[0] = 0;
        if (!HidD_GetManufacturerString(f, mfr, sizeof mfr)) mfr[0] = 0;
        CloseHandle(f);
    }
    unsigned vid = 0, pid = 0;
    const WCHAR *v = wcsstr(k->path, L"VID_"), *p = wcsstr(k->path, L"PID_");
    if (!v) v = wcsstr(k->path, L"vid_");
    if (!p) p = wcsstr(k->path, L"pid_");
    if (v) swscanf(v + 4, L"%4x", &vid);
    if (p) swscanf(p + 4, L"%4x", &pid);

    if (prod[0] && mfr[0] && !wcsstr(prod, mfr))
        _snwprintf(k->name, 159, L"%ls %ls", mfr, prod);
    else if (prod[0])
        lstrcpynW(k->name, prod, 160);
    else if (wcsstr(k->path, L"ACPI") || wcsstr(k->path, L"acpi"))
        lstrcpynW(k->name, L"Built-in / PS/2 keyboard", 160);
    else
        lstrcpynW(k->name, L"Keyboard", 160);
    k->name[159] = 0;

    /* short instance tag so identical keyboards can be told apart */
    const WCHAR *tail = k->path;
    int hashes = 0;
    for (const WCHAR *s = k->path; *s; s++) if (*s == L'#' && ++hashes == 2) { tail = s + 1; break; }
    WCHAR inst[24]; int n = 0;
    for (const WCHAR *s = tail; *s && *s != L'#' && n < 12; s++) inst[n++] = *s;
    inst[n] = 0;
    if (vid || pid) _snwprintf(k->detail, 95, L"VID %04X  PID %04X   #%ls", vid, pid, inst);
    else            _snwprintf(k->detail, 95, L"#%ls", inst);
    k->detail[95] = 0;
}

static Kbd *find_by_handle(HANDLE h)
{
    for (int i = 0; i < g_nkbd; i++) if (g_kbd[i].h == h && g_kbd[i].connected) return &g_kbd[i];
    return NULL;
}

static Kbd *add_device(HANDLE h)
{
    WCHAR path[512]; UINT sz = 512;
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, path, &sz) == (UINT)-1) return NULL;
    path[511] = 0;
    /* reuse a slot with the same path (replug) */
    for (int i = 0; i < g_nkbd; i++) {
        if (!lstrcmpiW(g_kbd[i].path, path)) {
            g_kbd[i].h = h; g_kbd[i].connected = 1;
            return &g_kbd[i];
        }
    }
    if (g_nkbd >= MAX_KBD) return NULL;
    Kbd *k = &g_kbd[g_nkbd++];
    memset(k, 0, sizeof *k);
    k->h = h;
    lstrcpynW(k->path, path, 512);
    k->connected = 1;
    k->player = GetPrivateProfileIntW(L"Assign", k->path, 1, g_iniPath) == 2 ? 2 : 1;
    if (k->player == 2) { k->shown = 1; k->order = ++g_orderCounter; }
    describe(k);
    return k;
}

static void enumerate_devices(void)
{
    UINT n = 0;
    GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST));
    if (!n) return;
    RAWINPUTDEVICELIST *list = (RAWINPUTDEVICELIST *)HeapAlloc(GetProcessHeap(), 0, n * sizeof *list);
    if (!list) return;
    n = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
    for (UINT i = 0; i < n && i != (UINT)-1; i++)
        if (list[i].dwType == RIM_TYPEKEYBOARD) add_device(list[i].hDevice);
    HeapFree(GetProcessHeap(), 0, list);
}

static void set_player(Kbd *k, int player)
{
    if (k->player == player) return;
    release_all(k);
    k->player = player;
    WritePrivateProfileStringW(L"Assign", k->path, player == 2 ? L"2" : L"1", g_iniPath);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ---- game launching ----------------------------------------------------- */
/* dgVoodoo is distributed unmodified as application-local DLLs. Keep an
 * existing different DLL as a backup; never silently overwrite that backup. */
static int unpack_file(const WCHAR *dir, const WCHAR *name, int id, int config)
{
    WCHAR path[MAX_PATH], backup[MAX_PATH];
    if (wcslen(dir) + wcslen(name) + 24 >= MAX_PATH) {
        set_status(L"Game folder path is too long."); return 0;
    }
    _snwprintf(path, MAX_PATH, L"%ls%ls", dir, name);
    if (config && file_exists(path)) return 1;
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL loaded = res ? LoadResource(NULL, res) : NULL;
    const void *data = loaded ? LockResource(loaded) : NULL;
    DWORD size = res ? SizeofResource(NULL, res) : 0;
    if (!data || !size) { set_status(L"Embedded component missing: %ls", name); return 0; }
    if (file_exists(path)) {
        HANDLE old = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        int same = 0;
        if (old != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            void *buf = HeapAlloc(GetProcessHeap(), 0, size);
            if (buf && GetFileSize(old, NULL) == size && ReadFile(old, buf, size, &got, NULL))
                same = got == size && !memcmp(buf, data, size);
            if (buf) HeapFree(GetProcessHeap(), 0, buf);
            CloseHandle(old);
        }
        if (same) return 1;
        _snwprintf(backup, MAX_PATH, L"%ls.pre-dgvoodoo", path);
        if (!CopyFileW(path, backup, TRUE)) {
            set_status(L"Cannot back up %ls (error %lu). Close the game or check the backup.", name, GetLastError());
            return 0;
        }
    }
    HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    int ok = out != INVALID_HANDLE_VALUE && WriteFile(out, data, size, &written, NULL) && written == size;
    DWORD err = GetLastError();
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) set_status(L"Cannot write %ls (error %lu). Use a writable game folder.", name, err);
    return ok;
}

static int prepare_graphics(const WCHAR *dir)
{
    return unpack_file(dir, L"DDraw.dll", 201, 0) &&
           unpack_file(dir, L"D3DImm.dll", 202, 0) &&
           unpack_file(dir, L"dgVoodoo.conf", 203, 1) &&
           unpack_file(dir, L"dinput.dll", 204, 0);
}

static int configure_display(const WCHAR *dir, uint32_t mode)
{
    WCHAR config[MAX_PATH], resolution[64];
    if (wcslen(dir) + 16 >= MAX_PATH) return 0;
    _snwprintf(config, MAX_PATH, L"%lsdgVoodoo.conf", dir);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int w = sw, h = sh;
    if (mode == TOD2P_DISPLAY_BORDERLESS) {
        w = sh * 4 / 3;
        if (w > sw) { w = sw; h = sw * 3 / 4; }
        w &= ~1; h &= ~1;
    } else if (mode == TOD2P_DISPLAY_WINDOW2X) { w = 1280; h = 960; }
    if (mode == TOD2P_DISPLAY_ORIGINAL) lstrcpyW(resolution, L"unforced");
    else _snwprintf(resolution, 64, L"h:%d, v:%d", w, h);
    int windowed = mode == TOD2P_DISPLAY_BORDERLESS || mode == TOD2P_DISPLAY_WINDOW2X;
    /* Own only settings needed by F3. Other graphics preferences are retained. */
    return WritePrivateProfileStringW(L"DirectX", L"Resolution", resolution, config) &&
        WritePrivateProfileStringW(L"DirectX", L"AppControlledScreenMode", L"false", config) &&
        WritePrivateProfileStringW(L"General", L"FullScreenMode", windowed ? L"false" : L"true", config) &&
        WritePrivateProfileStringW(L"General", L"ScalingMode", mode == TOD2P_DISPLAY_FULL_NATIVE ? L"stretched" : L"stretched_ar", config) &&
        WritePrivateProfileStringW(L"General", L"CenterAppWindow", L"true", config) &&
        WritePrivateProfileStringW(L"GeneralExt", L"WindowedAttributes", mode == TOD2P_DISPLAY_BORDERLESS ? L"borderless, fullscreensize" : L"", config);
}

static int browse_for_game(void)
{
    WCHAR file[MAX_PATH] = L"Tod_e.exe";
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Typing of the Dead (Tod_e.exe)\0Tod_e.exe;*.exe\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return 0;
    lstrcpynW(g_gameExe, file, MAX_PATH);
    WritePrivateProfileStringW(L"Game", L"Exe", g_gameExe, g_iniPath);
    return 1;
}

static void launch_game(void)
{
    if (g_gameProc) { set_status(L"The game is already running."); return; }
    if (!file_exists(g_gameExe) && !browse_for_game()) {
        set_status(L"Game not found. Press F2 to locate Tod_e.exe.");
        return;
    }
    WCHAR dir[MAX_PATH], dll[MAX_PATH];
    exe_dir(dir, g_gameExe);
    if (!prepare_graphics(dir)) return;
    if (!configure_display(dir, g_display)) {
        set_status(L"Cannot update dgVoodoo.conf for the selected display mode."); return;
    }
    _snwprintf(dll, MAX_PATH - 1, L"%lsdinput.dll", dir);
    dll[MAX_PATH - 1] = 0;
    if (!file_exists(dll)) {
        set_status(L"dinput.dll (TotD2P proxy) is missing next to the game: %ls", dir);
        return;
    }
    WCHAR cmd[MAX_PATH + 4];
    _snwprintf(cmd, MAX_PATH + 3, L"\"%ls\"", g_gameExe);
    cmd[MAX_PATH + 3] = 0;
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    /* Restrict the compatibility override to the child. No machine settings,
     * elevation, or registry writes are needed. */
    DWORD envSize = GetEnvironmentVariableW(L"__COMPAT_LAYER", NULL, 0);
    WCHAR *saved = envSize ? HeapAlloc(GetProcessHeap(), 0, envSize * sizeof(WCHAR)) : NULL;
    if (envSize && !saved) { set_status(L"Out of memory preparing game environment."); return; }
    if (saved) GetEnvironmentVariableW(L"__COMPAT_LAYER", saved, envSize);
    SetEnvironmentVariableW(L"__COMPAT_LAYER", L"RunAsInvoker");
    BOOL started = CreateProcessW(g_gameExe, cmd, NULL, NULL, FALSE, 0, NULL, dir[0] ? dir : NULL, &si, &pi);
    DWORD launchError = GetLastError();
    SetEnvironmentVariableW(L"__COMPAT_LAYER", saved);
    if (saved) HeapFree(GetProcessHeap(), 0, saved);
    if (!started) {
        set_status(L"Could not start the game (error %lu).", launchError);
        return;
    }
    CloseHandle(pi.hThread);
    g_gameProc = pi.hProcess;
    set_status(L"Game started. Keep this window open while you play.");
}

/* ---- raw input ---------------------------------------------------------- */
static void on_raw_input(HRAWINPUT hri)
{
    RAWINPUT ri; UINT sz = sizeof ri;
    if (GetRawInputData(hri, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    if (ri.header.dwType != RIM_TYPEKEYBOARD) return;

    const RAWKEYBOARD *kb = &ri.data.keyboard;
    if (kb->VKey == 0xFF || kb->MakeCode == 0 || kb->MakeCode == 0xFF) return; /* fake / overrun */

    int e0 = (kb->Flags & RI_KEY_E0) != 0;
    uint8_t dik;
    if (kb->Flags & RI_KEY_E1) {
        if ((kb->MakeCode & 0x7F) != 0x1D) return;
        dik = 0xC5;                                    /* Pause */
    } else {
        uint8_t mc = (uint8_t)(kb->MakeCode & 0x7F);
        if (e0 && (mc == 0x2A || mc == 0x36)) return;  /* fake shifts */
        dik = (uint8_t)(mc | (e0 ? 0x80 : 0));
    }
    int down = !(kb->Flags & RI_KEY_BREAK);
    DWORD t = GetMessageTime();

    Kbd *k = NULL;
    if (ri.header.hDevice) {
        k = find_by_handle(ri.header.hDevice);
        if (!k) k = add_device(ri.header.hDevice);
    }
    if (!k) {                         /* injected / unknown input -> P1 */
        static uint8_t virt[256];
        if (down == virt[dik]) return;
        virt[dik] = (uint8_t)down;
        player_key(1, dik, down, t);
        return;
    }

    if (!k->shown) { k->shown = 1; k->order = ++g_orderCounter; }
    if (down) k->lastActivity = GetTickCount();
    int wasDown = k->keys[dik];
    device_key(k, dik, down, t);

    /* launcher controls only while this window is in front */
    if (down && !wasDown && GetForegroundWindow() == g_hwnd) {
        switch (kb->VKey) {
        case VK_LEFT:  case 'A': set_player(k, 1); break;
        case VK_RIGHT: case 'D': set_player(k, 2); break;
        case VK_RETURN:          launch_game(); break;
        case VK_F2:              if (browse_for_game()) set_status(L"Game: %ls", g_gameExe); break;
        case VK_F3:
            set_display(g_display + 1);
            if (g_gameProc) set_status(L"Display mode applies the next time the game starts.");
            break;
        case VK_ESCAPE:
            if (!g_gameProc) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            break;
        }
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void on_device_change(WPARAM w, HANDLE h)
{
    if (w == GIDC_ARRIVAL) {
        RID_DEVICE_INFO info; UINT sz = sizeof info; info.cbSize = sizeof info;
        if (GetRawInputDeviceInfoW(h, RIDI_DEVICEINFO, &info, &sz) != (UINT)-1 &&
            info.dwType == RIM_TYPEKEYBOARD)
            add_device(h);
    } else if (w == GIDC_REMOVAL) {
        for (int i = 0; i < g_nkbd; i++)
            if (g_kbd[i].h == h && g_kbd[i].connected) {
                release_all(&g_kbd[i]);
                g_kbd[i].connected = 0;
            }
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ---- drawing ------------------------------------------------------------ */
#define C_BG      RGB(0x14,0x12,0x12)
#define C_PANEL   RGB(0x22,0x1E,0x1E)
#define C_CARD    RGB(0x30,0x2A,0x2A)
#define C_TEXT    RGB(0xEE,0xE8,0xE0)
#define C_DIM     RGB(0x9A,0x90,0x88)
#define C_P1      RGB(0x3A,0x9B,0xE8)
#define C_P2      RGB(0xE8,0x4A,0x3A)
#define C_GOOD    RGB(0x5C,0xC8,0x6A)
#define C_WARN    RGB(0xE8,0xB8,0x3A)

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void text(HDC dc, HFONT f, COLORREF c, int x, int y, int w, int h, const WCHAR *s, UINT fmt)
{
    RECT r = { x, y, x + w, y + h };
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    fill(dc, 0, 0, W, H, C_BG);
    text(dc, g_fTitle, C_TEXT, 24, 14, W - 48, 34, L"Typing of the Dead \x2014 2 Keyboards", DT_LEFT);
    text(dc, g_fSmall, C_DIM, 24, 48, W - 48, 20,
         L"Press any key on each keyboard to show it. Use \x2190/A or \x2192/D on a keyboard to move it.",
         DT_LEFT);

    int colW = (W - 24 * 3) / 2, top = 80, colH = H - top - 112;
    for (int pl = 1; pl <= 2; pl++) {
        int x = 24 + (pl - 1) * (colW + 24);
        fill(dc, x, top, colW, colH, C_PANEL);
        fill(dc, x, top, colW, 4, pl == 1 ? C_P1 : C_P2);
        text(dc, g_fBig, pl == 1 ? C_P1 : C_P2, x + 16, top + 12, colW - 32, 30,
             pl == 1 ? L"PLAYER 1" : L"PLAYER 2", DT_LEFT);

        /* cards in first-seen order */
        int y = top + 52, any = 0;
        for (int ord = 1; ord <= g_orderCounter; ord++) {
            for (int i = 0; i < g_nkbd; i++) {
                Kbd *k = &g_kbd[i];
                if (!k->shown || k->order != ord || k->player != pl) continue;
                if (y + 60 > top + colH) break;
                any = 1;
                fill(dc, x + 12, y, colW - 24, 56, C_CARD);
                DWORD age = GetTickCount() - k->lastActivity;
                COLORREF dot = !k->connected ? C_DIM : (age < 350 ? C_GOOD : RGB(0x55,0x4C,0x48));
                fill(dc, x + 24, y + 22, 12, 12, dot);
                text(dc, g_fName, k->connected ? C_TEXT : C_DIM, x + 48, y + 7, colW - 140, 24, k->name, DT_LEFT);
                text(dc, g_fSmall, C_DIM, x + 48, y + 31, colW - 140, 18,
                     k->connected ? k->detail : L"disconnected", DT_LEFT);
                text(dc, g_fSmall, C_DIM, x + colW - 96, y + 18, 72, 20,
                     pl == 1 ? L"D  >" : L"<  A", DT_RIGHT);
                y += 64;
            }
        }
        if (!any)
            text(dc, g_fSmall, C_DIM, x + 16, top + 60, colW - 32, 20,
                 pl == 1 ? L"(all other keyboards play as Player 1)" : L"(no keyboard yet)", DT_LEFT);
    }

    /* status area */
    int sy = H - 104;
    WCHAR line[256];
    DWORD now = GetTickCount();
    int external = !g_gameProc && (g_shm->dllFlags & TOD2P_DLL_LOADED) &&
                   (now - g_shm->dllHeartbeat) < 2000;
    if (g_gameProc || external) {
        uint32_t f = g_shm->dllFlags;
        int dllAlive = (now - g_shm->dllHeartbeat) < 2000;
        if (!(f & TOD2P_DLL_LOADED))
            lstrcpyW(line, L"Game running \x2014 waiting for the TotD2P dinput.dll to connect\x2026");
        else if (!(f & TOD2P_DLL_EXE_MATCH))
            lstrcpyW(line, L"Game running \x2014 unknown Tod_e.exe build: Player 2 disabled (P1 filtering only)");
        else if (!dllAlive || !(f & TOD2P_DLL_GAME_FOCUS))
            lstrcpyW(line, L"Game running \x2014 connected (game window not focused)");
        else
            _snwprintf(line, 255, L"Game running \x2014 2-keyboard mode active (%lu polls)", (unsigned long)g_shm->dllPolls);
        line[255] = 0;
        text(dc, g_fName, (f & TOD2P_DLL_EXE_MATCH) ? C_GOOD : C_WARN, 24, sy, W - 48, 24, line, DT_LEFT);
    } else {
        text(dc, g_fName, C_TEXT, 24, sy, W - 48, 24, L"Game not running", DT_LEFT);
    }
    {
        WCHAR dn[96], dl[128];
        display_name(g_display, dn, 96);
        _snwprintf(dl, 127, L"Display: %ls   [F3]", dn); dl[127] = 0;
        text(dc, g_fName, g_display ? C_P1 : C_DIM, 24, sy, W - 48, 24, dl, DT_RIGHT);
    }
    text(dc, g_fSmall, C_DIM, 24, sy + 28, W - 48, 20, g_status, DT_LEFT);
    text(dc, g_fSmall, C_DIM, 24, sy + 52, W - 48, 20,
         L"Enter: start game     F2: locate Tod_e.exe     F3: display mode     Esc: quit     (keys work while this window is in front)",
         DT_LEFT);
    _snwprintf(line, 255, L"Game: %ls", g_gameExe); line[255] = 0;
    text(dc, g_fSmall, C_DIM, 24, sy + 74, W - 48, 20, line, DT_LEFT | DT_PATH_ELLIPSIS);

    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

/* ---- window ------------------------------------------------------------- */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_INPUT:
        on_raw_input((HRAWINPUT)l);
        break;                                   /* fall through to DefWindowProc */
    case WM_INPUT_DEVICE_CHANGE:
        on_device_change(w, (HANDLE)l);
        return 0;
    case WM_TIMER:
        g_shm->heartbeat = GetTickCount();
        if (g_gameProc && WaitForSingleObject(g_gameProc, 0) == WAIT_OBJECT_0) {
            CloseHandle(g_gameProc);
            g_gameProc = NULL;
            g_shm->dllFlags = 0;
            set_status(L"Game closed.");
            SetForegroundWindow(h);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = 720; mm->ptMinTrackSize.y = 420;
        return 0;
    }
    case WM_CLOSE:
        if (g_gameProc &&
            MessageBoxW(h, L"The game is still running. Closing the launcher turns 2-keyboard mode off.\n\nClose anyway?",
                        L"TotD2P", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HFONT mkfont(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdLine, int show)
{
    (void)prev;
    SetProcessDPIAware();

    HANDLE single = CreateMutexW(NULL, TRUE, L"Local\\TotD2P_Launcher");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"TotD2PLauncher", NULL);
        if (other) SetForegroundWindow(other);
        return 0;
    }

    WCHAR self[MAX_PATH], dir[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    exe_dir(dir, self);
    _snwprintf(g_iniPath, MAX_PATH - 1, L"%lstod2p.ini", dir);

    WCHAR def[MAX_PATH];
    _snwprintf(def, MAX_PATH - 1, L"%lsTod_e.exe", dir);
    GetPrivateProfileStringW(L"Game", L"Exe", def, g_gameExe, MAX_PATH, g_iniPath);
    if (cmdLine && cmdLine[0]) {                    /* optional: path to game */
        WCHAR arg[MAX_PATH]; lstrcpynW(arg, cmdLine, MAX_PATH);
        WCHAR *a = arg; if (*a == L'"') { a++; WCHAR *q = wcschr(a, L'"'); if (q) *q = 0; }
        if (file_exists(a)) lstrcpynW(g_gameExe, a, MAX_PATH);
    }

    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(Tod2pShared), TOD2P_SHM_NAME);
    if (!g_map) { MessageBoxW(NULL, L"Could not create shared memory.", L"TotD2P", MB_ICONERROR); return 1; }
    g_shm = (Tod2pShared *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Tod2pShared));
    if (!g_shm) { MessageBoxW(NULL, L"Could not map shared memory.", L"TotD2P", MB_ICONERROR); return 1; }
    memset(g_shm, 0, sizeof *g_shm);
    g_shm->magic = TOD2P_MAGIC;
    g_shm->version = TOD2P_VERSION;
    g_shm->heartbeat = GetTickCount();
    g_shm->active = 1;
    g_display = (uint32_t)GetPrivateProfileIntW(L"Display", L"Mode", 0, g_iniPath) % TOD2P_DISPLAY_COUNT;
    g_shm->displayMode = g_display;

    g_fTitle = mkfont(26, FW_BOLD);
    g_fBig   = mkfont(20, FW_BOLD);
    g_fName  = mkfont(17, FW_SEMIBOLD);
    g_fSmall = mkfont(14, FW_NORMAL);

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    wc.lpszClassName = L"TotD2PLauncher";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"TotD2P \x2014 Typing of the Dead 2 Keyboards",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 560,
                             NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    RAWINPUTDEVICE rid = { 0x01, 0x06, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, g_hwnd };
    if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) {
        rid.dwFlags = RIDEV_INPUTSINK;                 /* pre-Vista fallback */
        if (!RegisterRawInputDevices(&rid, 1, sizeof rid))
            MessageBoxW(g_hwnd, L"Raw Input registration failed; keyboards can't be told apart.",
                        L"TotD2P", MB_ICONWARNING);
    }
    enumerate_devices();
    set_status(file_exists(g_gameExe) ? L"Ready." : L"Tod_e.exe not found next to the launcher \x2014 press F2 to locate it.");

    SetTimer(g_hwnd, TIMER_ID, TIMER_MS, NULL);
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_shm->active = 0;
    UnmapViewOfFile(g_shm);
    CloseHandle(g_map);
    if (single) CloseHandle(single);
    return 0;
}

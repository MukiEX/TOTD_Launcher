/*
 * TotD2P display modes for Tod_e.exe (PE timestamp 0x3A1CF9B4).
 *
 * The HGL engine already supports arbitrary resolutions: its set-mode routine
 * (0x4BD6A0) takes width/height/bpp/fullscreen from a mode table and updates
 * the D3DX context, the full-screen quad and the window. The game just
 * hardcodes table entry 11 (640x480x16, fullscreen) and re-forces 640x480
 * right before creating the window/context. We patch, before WinMain runs:
 *   - mode table entry 11  (0x5EB6C8 + 11*16): width, height, bpp, fullscreen
 *   - the two 640/480 immediates at 0x4BDE38 / 0x4BDE42
 *   - windowed modes: "FullScreen=T" in hglCore.ini is ignored (0x4BE936)
 * and hook CreateWindowExA / MoveWindow / SetWindowPos for the borderless
 * 4:3 mode (popup window centred over a black backdrop).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "../common/tod2p_shm.h"

void tod2p_log(const char *fmt, ...);

#define MODE_ENTRY        (0x005EB6C8u + 11u * 16u)
#define IMM_WIDTH         0x004BDE38u   /* mov [0x8490AC], 640 -> imm32 */
#define IMM_HEIGHT        0x004BDE42u   /* mov [0x8490B0], 480 -> imm32 */
#define FULLSCREEN_INI_OR 0x004BE936u   /* or al, 4 -> imm8              */

static int  g_mode;
static int  g_winX, g_winY, g_winW, g_winH;   /* forced outer rect (borderless) */
static int  g_monX, g_monY, g_monW, g_monH;
static HWND g_game, g_backdrop;

typedef HWND (WINAPI *PFN_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                           HWND, HMENU, HINSTANCE, LPVOID);
typedef BOOL (WINAPI *PFN_MoveWindow)(HWND, int, int, int, int, BOOL);
typedef BOOL (WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static PFN_CreateWindowExA r_CreateWindowExA;
static PFN_MoveWindow      r_MoveWindow;
static PFN_SetWindowPos    r_SetWindowPos;

static int patch_bytes(uintptr_t addr, const void *src, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void *)addr, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)addr, src, n);
    VirtualProtect((void *)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, n);
    return 1;
}

static void *hook_iat(const char *dll, const char *func, void *repl)
{
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((const char *)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA32 *names = (IMAGE_THUNK_DATA32 *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32 *addrs = (IMAGE_THUNK_DATA32 *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)n->Name, func)) continue;
            void *orig = (void *)(uintptr_t)addrs->u1.Function;
            uint32_t v = (uint32_t)(uintptr_t)repl;
            if (patch_bytes((uintptr_t)&addrs->u1.Function, &v, 4)) return orig;
            return NULL;
        }
    }
    return NULL;
}

/* ---- borderless backdrop ------------------------------------------------ */
static LRESULT CALLBACK backdrop_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE && g_game) { SetActiveWindow(g_game); SetFocus(g_game); }
        return 0;
    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;
    case WM_CLOSE:
        if (g_game) PostMessageA(g_game, WM_CLOSE, 0, 0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static HWND create_backdrop(HINSTANCE inst, LPCSTR title)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = backdrop_proc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(101));
    wc.lpszClassName = "TotD2PBackdrop";
    RegisterClassA(&wc);
    return r_CreateWindowExA(0, wc.lpszClassName, title ? title : "The Typing of The Dead",
                             WS_POPUP | WS_VISIBLE, g_monX, g_monY, g_monW, g_monH,
                             NULL, NULL, inst, NULL);
}

/* ---- window hooks ------------------------------------------------------- */
static HWND WINAPI hk_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style,
                                      int x, int y, int w, int h, HWND parent, HMENU menu,
                                      HINSTANCE inst, LPVOID param)
{
    int isGame = !g_game && !parent && !(style & WS_CHILD) &&
                 HIWORD((uintptr_t)cls) && !lstrcmpA(cls, "HGLApp");
    if (!isGame) return r_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);

    SetProcessDPIAware();     /* no DPI virtualisation blur in windowed modes */

    if (g_mode == TOD2P_DISPLAY_BORDERLESS) {
        g_backdrop = create_backdrop(inst, title);
        style = (style & WS_VISIBLE) | WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        ex = 0;
        x = g_winX; y = g_winY; w = g_winW; h = g_winH;
        parent = g_backdrop;  /* owner: keeps the game above the backdrop */
    } else if (g_mode == TOD2P_DISPLAY_WINDOW2X) {
        RECT r = { 0, 0, 1280, 960 };
        AdjustWindowRectEx(&r, style, FALSE, ex);
        w = r.right - r.left; h = r.bottom - r.top;
        x = g_monX + (g_monW - w) / 2; y = g_monY + (g_monH - h) / 2;
        if (y < g_monY) y = g_monY;
    }
    g_game = r_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    tod2p_log("game window %p created (%d,%d %dx%d) style 0x%08lx", (void *)g_game, x, y, w, h, style);
    return g_game;
}

static BOOL WINAPI hk_MoveWindow(HWND hw, int x, int y, int w, int h, BOOL repaint)
{
    if (hw && hw == g_game) {
        if (g_mode == TOD2P_DISPLAY_BORDERLESS) {
            x = g_winX; y = g_winY; w = g_winW; h = g_winH;
        } else if (g_mode == TOD2P_DISPLAY_WINDOW2X) {
            RECT r = { 0, 0, 1280, 960 };
            AdjustWindowRectEx(&r, (DWORD)GetWindowLongA(hw, GWL_STYLE), FALSE,
                               (DWORD)GetWindowLongA(hw, GWL_EXSTYLE));
            w = r.right - r.left; h = r.bottom - r.top;
        }
    }
    return r_MoveWindow(hw, x, y, w, h, repaint);
}

static BOOL WINAPI hk_SetWindowPos(HWND hw, HWND after, int x, int y, int w, int h, UINT f)
{
    if (hw && hw == g_game && g_mode == TOD2P_DISPLAY_BORDERLESS)
        f |= SWP_NOMOVE | SWP_NOSIZE;
    return r_SetWindowPos(hw, after, x, y, w, h, f);
}

/* ---- entry -------------------------------------------------------------- */
void tod2p_display_init(int mode)
{
    if (mode <= (int)TOD2P_DISPLAY_ORIGINAL || mode >= (int)TOD2P_DISPLAY_COUNT) return;
    g_mode = mode;

    /* physical desktop size of the primary display (DPI-independent) */
    DEVMODEA dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth && dm.dmPelsHeight) {
        g_monX = dm.dmPosition.x; g_monY = dm.dmPosition.y;
        g_monW = (int)dm.dmPelsWidth; g_monH = (int)dm.dmPelsHeight;
    } else {
        g_monX = g_monY = 0; g_monW = 1920; g_monH = 1080;
    }

    uint32_t w, h, bpp = 32, fullscreen;
    switch (mode) {
    case TOD2P_DISPLAY_FULL_NATIVE:
        w = (uint32_t)g_monW; h = (uint32_t)g_monH; fullscreen = 1;
        break;
    case TOD2P_DISPLAY_BORDERLESS:
        h = (uint32_t)g_monH; w = h * 4 / 3;
        if (w > (uint32_t)g_monW) { w = (uint32_t)g_monW; h = w * 3 / 4; }
        w &= ~1u; h &= ~1u;
        g_winW = (int)w; g_winH = (int)h;
        g_winX = g_monX + (g_monW - g_winW) / 2;
        g_winY = g_monY + (g_monH - g_winH) / 2;
        fullscreen = 0;
        break;
    default: /* TOD2P_DISPLAY_WINDOW2X */
        w = 1280; h = 960; fullscreen = 0;
        break;
    }

    /* sanity: confirm the bytes we're about to patch are the stock ones */
    const uint32_t *entry = (const uint32_t *)MODE_ENTRY;
    if (entry[0] != 640 || entry[1] != 480 || *(const uint32_t *)IMM_WIDTH != 640 ||
        *(const uint32_t *)IMM_HEIGHT != 480 || *(const uint8_t *)FULLSCREEN_INI_OR != 4) {
        tod2p_log("display: unexpected bytes, mode %d NOT applied", mode);
        g_mode = 0;
        return;
    }

    uint32_t e[4] = { w, h, bpp, fullscreen };
    patch_bytes(MODE_ENTRY, e, sizeof e);
    patch_bytes(IMM_WIDTH, &w, 4);
    patch_bytes(IMM_HEIGHT, &h, 4);
    if (!fullscreen) { uint8_t z = 0; patch_bytes(FULLSCREEN_INI_OR, &z, 1); }

    r_CreateWindowExA = (PFN_CreateWindowExA)hook_iat("USER32.dll", "CreateWindowExA", (void *)hk_CreateWindowExA);
    r_MoveWindow      = (PFN_MoveWindow)hook_iat("USER32.dll", "MoveWindow", (void *)hk_MoveWindow);
    r_SetWindowPos    = (PFN_SetWindowPos)hook_iat("USER32.dll", "SetWindowPos", (void *)hk_SetWindowPos);
    if (!r_CreateWindowExA) r_CreateWindowExA = CreateWindowExA;
    if (!r_MoveWindow)      r_MoveWindow = MoveWindow;
    if (!r_SetWindowPos)    r_SetWindowPos = SetWindowPos;

    tod2p_log("display: mode %d -> %lux%lu %s (desktop %dx%d)", mode,
              (unsigned long)w, (unsigned long)h, fullscreen ? "fullscreen" : "windowed", g_monW, g_monH);
}

int tod2p_display_active(void) { return g_mode != 0; }

/*
 * Test harness: impersonates Tod_e.exe (PE timestamp patched after link,
 * memory mapped at the game's port-B / DIK table addresses) and acts as the
 * launcher by writing the shared memory directly. Exercises the proxy
 * dinput.dll through the real DirectInput keyboard device.
 */
#define DIRECTINPUT_VERSION 0x0700
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../common/tod2p_shm.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   "); else { printf("  FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static LPDIRECTINPUTDEVICE7A kbd;
static volatile uint8_t *portB = (uint8_t *)0x847F10;
static Tod2pShared *shm;
static uint8_t g_span[0x500000];

static void poll(DIDEVICEOBJECTDATA *ev, DWORD *n, BYTE *state, HRESULT *hrD, HRESULT *hrS)
{
    *n = 8;
    *hrD = IDirectInputDevice7_GetDeviceData(kbd, sizeof(DIDEVICEOBJECTDATA), ev, n, 0);
    *hrS = IDirectInputDevice7_GetDeviceState(kbd, 256, state);
}

static void push(uint8_t dik, int down)
{
    uint32_t w = shm->p1Write;
    shm->p1ring[w & (TOD2P_RING - 1)].dik = dik;
    shm->p1ring[w & (TOD2P_RING - 1)].down = (uint8_t)down;
    shm->p1ring[w & (TOD2P_RING - 1)].time = GetTickCount();
    shm->p1Write = w + 1;
    shm->p1state[dik] = down ? 0x80 : 0;
}

static void dumpB(const char *tag)
{
    printf("       port B %-10s: %02X %02X | %02X %02X %02X %02X %02X %02X\n", tag,
           portB[0], portB[1], portB[2], portB[3], portB[4], portB[5], portB[6], portB[7]);
}

static int e2e(void);
static int display_test(int mode);
int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "e2e")) return e2e();
    if (argc > 2 && !strcmp(argv[1], "display")) return display_test(atoi(argv[2]));
    /* game memory: port B buffer + DIK->HID table live in the same page */
    /* big .bss makes the exe image itself span 0x847000 (like the real game's .data) */
    if ((uintptr_t)g_span > 0x847000 || (uintptr_t)g_span + sizeof g_span < 0x848000) {
        printf("image does not span 0x847000 (%p)\n", (void *)g_span); return 2; }
    uint8_t *tbl = (uint8_t *)0x8479F0;
    static const uint8_t pairs[][2] = { {0x1E,0x04},{0x1C,0x28},{0x39,0x2C},{0x20,0x07},
        {0x10,0x14},{0x11,0x1A},{0x12,0x08},{0x13,0x15},{0x14,0x17},{0x15,0x1C},{0x16,0x18},{0x17,0x0C} };
    for (unsigned i = 0; i < sizeof pairs / 2; i++) tbl[pairs[i][0] * 4] = pairs[i][1];
    memset((void *)portB, 0xEE, 8);

    HMODULE dll = LoadLibraryA(".\\dinput.dll");
    if (!dll) { printf("could not load proxy\n"); return 2; }
    HRESULT (WINAPI *create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN) =
        (void *)GetProcAddress(dll, "DirectInputCreateEx");

    HWND hwnd = CreateWindowA("STATIC", "fakegame", WS_OVERLAPPED, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    LPDIRECTINPUT7A di;
    HRESULT hr = create(GetModuleHandle(NULL), 0x700, &IID_IDirectInput7A, (void **)&di, NULL);
    CHECK(SUCCEEDED(hr), "DirectInputCreateEx via proxy (0x%08lx)", hr);
    hr = IDirectInput7_CreateDeviceEx(di, &GUID_SysKeyboard, &IID_IDirectInputDevice7A, (void **)&kbd, NULL);
    CHECK(SUCCEEDED(hr), "CreateDeviceEx(SysKeyboard) (0x%08lx)", hr);
    IDirectInputDevice7_SetDataFormat(kbd, &c_dfDIKeyboard);
    IDirectInputDevice7_SetCooperativeLevel(kbd, hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    DIPROPDWORD p = { { sizeof p, sizeof p.diph, 0, DIPH_DEVICE }, 128 };
    IDirectInputDevice7_SetProperty(kbd, DIPROP_BUFFERSIZE, &p.diph);
    hr = IDirectInputDevice7_Acquire(kbd);
    CHECK(SUCCEEDED(hr), "Acquire (0x%08lx)", hr);

    DIDEVICEOBJECTDATA ev[8]; DWORD n; BYTE st[256]; HRESULT hd, hs;

    printf("[1] no launcher -> passthrough\n");
    poll(ev, &n, st, &hd, &hs);
    CHECK(SUCCEEDED(hd) && SUCCEEDED(hs), "real calls succeed (data 0x%08lx state 0x%08lx)", hd, hs);
    CHECK(portB[0] == 0xEE, "port B untouched");

    printf("[2] launcher appears\n");
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(Tod2pShared), TOD2P_SHM_NAME);
    shm = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Tod2pShared));
    memset(shm, 0, sizeof *shm);
    shm->magic = TOD2P_MAGIC; shm->version = TOD2P_VERSION;
    shm->heartbeat = GetTickCount(); shm->active = 1;
    push(0x30, 1); push(0x30, 0);                     /* stale history: must be skipped */
    Sleep(1100);
    shm->heartbeat = GetTickCount();
    poll(ev, &n, st, &hd, &hs);
    CHECK(n == 0, "history before connect not replayed (n=%lu)", n);
    CHECK(portB[0] == 0 && portB[2] == 0, "port B cleared to idle");
    CHECK(shm->dllFlags & TOD2P_DLL_EXE_MATCH, "DLL reports known exe build (flags 0x%lx)", shm->dllFlags);
    CHECK(shm->dllFlags & TOD2P_DLL_KBD_HOOKED, "DLL reports keyboard hooked");

    printf("[3] player 1 events\n");
    push(0x1E, 1); push(0x1E, 0); push(0x12, 1);
    poll(ev, &n, st, &hd, &hs);
    CHECK(n == 3, "3 buffered events (n=%lu)", n);
    CHECK(n >= 3 && ev[0].dwOfs == 0x1E && ev[0].dwData == 0x80 && ev[1].dwOfs == 0x1E && ev[1].dwData == 0
          && ev[2].dwOfs == 0x12 && ev[2].dwData == 0x80, "A down, A up, E down in order");
    CHECK(st[0x12] == 0x80 && st[0x1E] == 0, "state: E held, A released");
    poll(ev, &n, st, &hd, &hs);
    CHECK(n == 0, "queue drained (n=%lu)", n);

    printf("[4] player 2 -> port B\n");
    shm->p2state[0x2A] = 0x80; shm->p2state[0x1C] = 0x80; shm->p2state[0x20] = 0x80;
    poll(ev, &n, st, &hd, &hs); dumpB("shift+D+Ent");
    CHECK(portB[0] == 0x02 && portB[2] == 0x28 && portB[3] == 0x07 && portB[4] == 0, "LShift bit, Enter + D");
    CHECK(st[0x20] == 0 && st[0x1C] == 0 && st[0x2A] == 0, "P2 keys absent from P1 state");
    shm->p2state[0x1C] = 0; shm->p2state[0x39] = 0x80;
    poll(ev, &n, st, &hd, &hs); dumpB("D+Space");
    CHECK(portB[2] == 0x07 && portB[3] == 0x2C, "held D keeps its slot, Space appended");
    for (int d = 0x10; d <= 0x17; d++) shm->p2state[d] = 0x80;
    poll(ev, &n, st, &hd, &hs); dumpB("rollover");
    CHECK(portB[2] == 0x01 && portB[7] == 0x01, "more than 6 keys -> ErrorRollOver");
    memset((void *)shm->p2state, 0, 256);
    poll(ev, &n, st, &hd, &hs); dumpB("released");
    CHECK(portB[0] == 0 && portB[2] == 0 && portB[7] == 0, "all released");

    printf("[5] launcher dies with P2 key held\n");
    shm->p2state[0x1E] = 0x80;
    poll(ev, &n, st, &hd, &hs);
    CHECK(portB[2] == 0x04, "P2 'A' reported");
    shm->heartbeat = GetTickCount() - 10000;
    poll(ev, &n, st, &hd, &hs); dumpB("stale");
    CHECK(portB[2] == 0, "stale launcher -> P2 released");

    printf("[6] ring overflow\n");
    shm->heartbeat = GetTickCount();
    poll(ev, &n, st, &hd, &hs);
    for (int i = 0; i < 600; i++) push(0x1E, i & 1 ? 0 : 1);
    n = 8;
    hr = IDirectInputDevice7_GetDeviceData(kbd, sizeof(DIDEVICEOBJECTDATA), ev, &n, 0);
    CHECK(hr == DI_BUFFEROVERFLOW && n == 8, "DI_BUFFEROVERFLOW reported (hr=0x%lx n=%lu)", hr, n);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

/* end-to-end: launcher owns the shared memory; just poll and report */
static int e2e(void)
{
    memset((void *)portB, 0, 8);
    uint8_t *tbl = (uint8_t *)0x8479F0;
    tbl[0x1E*4]=0x04; tbl[0x1C*4]=0x28; tbl[0x39*4]=0x2C; tbl[0x23*4]=0x0B; tbl[0x17*4]=0x0C;
    HMODULE dll = LoadLibraryA(".\\dinput.dll");
    HRESULT (WINAPI *create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN) = (void *)GetProcAddress(dll, "DirectInputCreateEx");
    HWND hwnd = CreateWindowA("STATIC", "fakegame", WS_OVERLAPPED, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    LPDIRECTINPUT7A di;
    create(GetModuleHandle(NULL), 0x700, &IID_IDirectInput7A, (void **)&di, NULL);
    IDirectInput7_CreateDeviceEx(di, &GUID_SysKeyboard, &IID_IDirectInputDevice7A, (void **)&kbd, NULL);
    IDirectInputDevice7_SetDataFormat(kbd, &c_dfDIKeyboard);
    IDirectInputDevice7_SetCooperativeLevel(kbd, hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    DIPROPDWORD p = { { sizeof p, sizeof p.diph, 0, DIPH_DEVICE }, 128 };
    IDirectInputDevice7_SetProperty(kbd, DIPROP_BUFFERSIZE, &p.diph);
    IDirectInputDevice7_Acquire(kbd);
    uint8_t last[8] = {0};
    DWORD end = GetTickCount() + 15000;
    while (GetTickCount() < end) {
        DIDEVICEOBJECTDATA ev[8]; DWORD n; BYTE st[256]; HRESULT hd, hs;
        poll(ev, &n, st, &hd, &hs);
        for (DWORD i = 0; i < n; i++) printf("P1 event dik=%02lX %s\n", ev[i].dwOfs, ev[i].dwData ? "down" : "up");
        if (memcmp(last, (void *)portB, 8)) { memcpy(last, (void *)portB, 8); dumpB("P2"); }
        fflush(stdout);
        MSG m; while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
        Sleep(16);
    }
    return 0;
}

/* display-mode patches: plant stock bytes at the game's addresses, then load */
static int display_test(int mode)
{
    if ((uintptr_t)g_span > 0x4BDE00 || (uintptr_t)g_span + sizeof g_span < 0x5EB800) {
        printf("image does not span the patch addresses (%p)\n", (void *)g_span); return 2; }
    uint32_t stock[4] = { 640, 480, 16, 1 };
    memcpy((void *)0x5EB778, stock, 16);                       /* entry 11 */
    static const uint8_t mw[10] = {0xC7,0x05,0xAC,0x90,0x84,0x00,0x80,0x02,0x00,0x00};
    static const uint8_t mh[10] = {0xC7,0x05,0xB0,0x90,0x84,0x00,0xE0,0x01,0x00,0x00};
    memcpy((void *)0x4BDE32, mw, 10); memcpy((void *)0x4BDE3C, mh, 10);
    *(uint8_t *)0x4BE935 = 0x0C; *(uint8_t *)0x4BE936 = 0x04;

    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(Tod2pShared), TOD2P_SHM_NAME);
    shm = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Tod2pShared));
    memset(shm, 0, sizeof *shm);
    shm->magic = TOD2P_MAGIC; shm->version = TOD2P_VERSION;
    shm->heartbeat = GetTickCount(); shm->active = 1; shm->displayMode = (uint32_t)mode;

    if (!LoadLibraryA(".\\dinput.dll")) { printf("could not load proxy\n"); return 2; }
    const uint32_t *e = (const uint32_t *)0x5EB778;
    printf("mode %d: entry11 = %lux%lu bpp %lu fs %lu | imm w=%lu h=%lu | ini-or=%u | flags 0x%lx\n", mode,
           e[0], e[1], e[2], e[3], *(uint32_t *)0x4BDE38, *(uint32_t *)0x4BDE42, *(uint8_t *)0x4BE936, shm->dllFlags);

    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "HGLApp"; RegisterClassA(&wc);
    /* same call shape as the game: overlapped style, CW_USEDEFAULT pos, 640x480 + frame */
    HWND h = CreateWindowExA(0, "HGLApp", "The Typing of The Dead", 0x02CE0000,
                             (int)0x80000000, (int)0x80000000, 646, 505, NULL, NULL, wc.hInstance, NULL);
    MoveWindow(h, 10, 10, 646, 505, TRUE);
    RECT wr, cr; GetWindowRect(h, &wr); GetClientRect(h, &cr);
    HWND owner = GetWindow(h, GW_OWNER);
    printf("  window %ldx%ld at (%ld,%ld), client %ldx%ld, style 0x%08lx, owner %s\n",
           wr.right - wr.left, wr.bottom - wr.top, wr.left, wr.top, cr.right, cr.bottom,
           (unsigned long)GetWindowLongA(h, GWL_STYLE), owner ? "backdrop" : "none");
    if (owner) { RECT br; GetWindowRect(owner, &br);
        printf("  backdrop %ldx%ld at (%ld,%ld)\n", br.right - br.left, br.bottom - br.top, br.left, br.top); }
    return 0;
}

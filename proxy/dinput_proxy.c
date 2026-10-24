/*
 * TotD2P proxy dinput.dll for The Typing of the Dead (PC, Tod_e.exe, 2000-11-23)
 *
 * Drop next to Tod_e.exe. Forwards everything to the real system dinput.dll,
 * but hooks the system keyboard device:
 *   - GetDeviceData / GetDeviceState return Player 1's keys only, rebuilt
 *     from the launcher's per-keyboard Raw Input state (shared memory).
 *   - Player 2's keys are written as a Dreamcast keyboard condition block
 *     into the game's emulated Maple port B buffer (0x847F10), which the game
 *     already registers as a connected keyboard but never fills offline.
 *
 * If the launcher isn't running (no/stale shared memory) everything passes
 * straight through and the game behaves exactly as stock.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "../common/tod2p_shm.h"

/* ---- Tod_e.exe (PE timestamp 0x3A1CF9B4) addresses ---------------------- */
#define TOD_EXE_TIMESTAMP   0x3A1CF9B4u
#define TOD_PORTB_COND      0x00847F10u  /* 8 bytes: mods, leds, key[6]      */
#define TOD_DIK2HID_TABLE   0x008479F0u  /* game's DIK->HID map, stride 4    */

/* ---- vtable slots -------------------------------------------------------- */
#define DI_SLOT_CREATEDEVICE     3   /* IDirectInput*::CreateDevice        */
#define DI7_SLOT_CREATEDEVICEEX  9   /* IDirectInput7::CreateDeviceEx      */
#define DID_SLOT_GETDEVICESTATE  9   /* IDirectInputDevice*::GetDeviceState */
#define DID_SLOT_GETDEVICEDATA  10   /* IDirectInputDevice*::GetDeviceData  */

#define DIGDD_PEEK          0x00000001
#define DI_OK_              ((HRESULT)0)
#define DI_BUFFEROVERFLOW_  ((HRESULT)1)

typedef struct { DWORD dwOfs, dwData, dwTimeStamp, dwSequence; } DIOBJDATA7;

static const GUID GUID_SysKeyboard_ =
    {0x6F1D2B61,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID IID_IDirectInput7A_ =
    {0x9A4CB684,0x236D,0x11D3,{0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE}};
static const GUID IID_IDirectInput7W_ =
    {0x9A4CB685,0x236D,0x11D3,{0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE}};

typedef HRESULT (WINAPI *PFN_DICreateEx)(HINSTANCE, DWORD, REFIID, LPVOID*, void *);
typedef HRESULT (WINAPI *PFN_DICreate)(HINSTANCE, DWORD, LPVOID*, void *);
typedef HRESULT (WINAPI *PFN_Void)(void);
typedef HRESULT (WINAPI *PFN_GetClassObject)(REFCLSID, REFIID, LPVOID*);

typedef HRESULT (WINAPI *PFN_CreateDevice)(void*, REFGUID, void**, void*);
typedef HRESULT (WINAPI *PFN_CreateDeviceEx)(void*, REFGUID, REFIID, void**, void*);
typedef HRESULT (WINAPI *PFN_GetDeviceState)(void*, DWORD, void*);
typedef HRESULT (WINAPI *PFN_GetDeviceData)(void*, DWORD, void*, DWORD*, DWORD);

static HMODULE g_real;
static PFN_DICreateEx     p_DirectInputCreateEx;
static PFN_DICreate       p_DirectInputCreateA, p_DirectInputCreateW;
static PFN_Void           p_DllCanUnloadNow, p_DllRegisterServer, p_DllUnregisterServer;
static PFN_GetClassObject p_DllGetClassObject;

static PFN_CreateDevice   o_CreateDevice;
static PFN_CreateDeviceEx o_CreateDeviceEx;
static PFN_GetDeviceState o_GetDeviceState;
static PFN_GetDeviceData  o_GetDeviceData;

static void *g_kbd;           /* the game's system keyboard device */
static int   g_exeMatch;      /* known build: safe to poke port B */

static HANDLE        g_map;
static Tod2pShared  *g_shm;
static DWORD         g_lastOpenTry;
static int           g_wasLive;
static uint32_t      g_p1Read;
static DWORD         g_seq;

static uint8_t g_dik2hid[256];
static int     g_tableReady;
static uint8_t g_p2keys[6];   /* keys currently reported on port B, in order */

static FILE *g_log;

/* ------------------------------------------------------------------------ */
static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

void tod2p_log(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

void tod2p_display_init(int mode);

static void open_log(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n-1] != '\\' && path[n-1] != '/') n--;
    path[n] = 0;
    strncat(path, "tod2p.log", MAX_PATH - strlen(path) - 1);
    g_log = fopen(path, "w");
}

static int load_real(void)
{
    if (g_real) return 1;
    WCHAR path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH); /* SysWOW64 under WOW64 */
    if (!n || n > MAX_PATH - 16) return 0;
    lstrcatW(path, L"\\dinput.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) { logf_("ERROR: could not load system dinput.dll"); return 0; }
    p_DirectInputCreateEx = (PFN_DICreateEx)GetProcAddress(g_real, "DirectInputCreateEx");
    p_DirectInputCreateA  = (PFN_DICreate)GetProcAddress(g_real, "DirectInputCreateA");
    p_DirectInputCreateW  = (PFN_DICreate)GetProcAddress(g_real, "DirectInputCreateW");
    p_DllCanUnloadNow     = (PFN_Void)GetProcAddress(g_real, "DllCanUnloadNow");
    p_DllRegisterServer   = (PFN_Void)GetProcAddress(g_real, "DllRegisterServer");
    p_DllUnregisterServer = (PFN_Void)GetProcAddress(g_real, "DllUnregisterServer");
    p_DllGetClassObject   = (PFN_GetClassObject)GetProcAddress(g_real, "DllGetClassObject");
    logf_("system dinput.dll loaded");
    return 1;
}

static void *patch_slot(void *obj, int idx, void *fn)
{
    void **vt = *(void ***)obj;
    if (vt[idx] == fn) return NULL;           /* already ours */
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return NULL;
    void *orig = vt[idx];
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vt[idx], sizeof(void*));
    return orig;
}

static void write_port_b(const volatile uint8_t *state);

/* ---- shared memory ------------------------------------------------------ */
static Tod2pShared *shm_live(void)
{
    DWORD now = GetTickCount();
    if (!g_shm && now - g_lastOpenTry > 1000) {
        g_lastOpenTry = now;
        g_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, TOD2P_SHM_NAME);
        if (g_map) {
            g_shm = (Tod2pShared *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Tod2pShared));
            if (!g_shm || g_shm->magic != TOD2P_MAGIC || g_shm->version != TOD2P_VERSION) {
                if (g_shm) UnmapViewOfFile(g_shm);
                CloseHandle(g_map);
                g_shm = NULL; g_map = NULL;
            } else {
                logf_("connected to launcher shared memory");
            }
        }
    }
    if (!g_shm) return NULL;

    g_shm->dllHeartbeat = now;
    g_shm->dllFlags |= TOD2P_DLL_LOADED | (g_kbd ? TOD2P_DLL_KBD_HOOKED : 0)
                     | (g_exeMatch ? TOD2P_DLL_EXE_MATCH : 0);

    int live = g_shm->active && (now - g_shm->heartbeat) < TOD2P_STALE_MS;
    if (live && !g_wasLive) {
        g_p1Read = g_shm->p1Write;            /* don't replay history */
        logf_("launcher live: taking over keyboard");
    } else if (!live && g_wasLive) {
        logf_("launcher gone/stale: passthrough");
        write_port_b(NULL);                   /* don't leave P2 keys stuck */
    }
    g_wasLive = live;
    return live ? g_shm : NULL;
}

/* ---- DIK -> HID --------------------------------------------------------- */
static void build_fallback_table(void)
{
    static const uint8_t pairs[][2] = {
        {0x01,0x29},{0x02,0x1E},{0x03,0x1F},{0x04,0x20},{0x05,0x21},{0x06,0x22},
        {0x07,0x23},{0x08,0x24},{0x09,0x25},{0x0A,0x26},{0x0B,0x27},{0x0C,0x2D},
        {0x0D,0x2E},{0x0E,0x2A},{0x0F,0x2B},{0x10,0x14},{0x11,0x1A},{0x12,0x08},
        {0x13,0x15},{0x14,0x17},{0x15,0x1C},{0x16,0x18},{0x17,0x0C},{0x18,0x12},
        {0x19,0x13},{0x1A,0x2F},{0x1B,0x30},{0x1C,0x28},{0x1E,0x04},{0x1F,0x16},
        {0x20,0x07},{0x21,0x09},{0x22,0x0A},{0x23,0x0B},{0x24,0x0D},{0x25,0x0E},
        {0x26,0x0F},{0x27,0x33},{0x28,0x34},{0x29,0x35},{0x2B,0x31},{0x2C,0x1D},
        {0x2D,0x1B},{0x2E,0x06},{0x2F,0x19},{0x30,0x05},{0x31,0x11},{0x32,0x10},
        {0x33,0x36},{0x34,0x37},{0x35,0x38},{0x37,0x55},{0x39,0x2C},{0x3A,0x39},
        {0x3B,0x3A},{0x3C,0x3B},{0x3D,0x3C},{0x3E,0x3D},{0x3F,0x3E},{0x40,0x3F},
        {0x41,0x40},{0x42,0x41},{0x43,0x42},{0x44,0x43},{0x45,0x53},{0x46,0x47},
        {0x47,0x5F},{0x48,0x60},{0x49,0x61},{0x4A,0x56},{0x4B,0x5C},{0x4C,0x5D},
        {0x4D,0x5E},{0x4E,0x57},{0x4F,0x59},{0x50,0x5A},{0x51,0x5B},{0x52,0x62},
        {0x53,0x63},{0x56,0x64},{0x57,0x44},{0x58,0x45},{0x70,0x88},{0x73,0x87},
        {0x79,0x8A},{0x7B,0x8B},{0x7D,0x89},{0x9C,0x58},{0xB5,0x54},{0xB7,0x46},
        {0xC5,0x48},{0xC7,0x4A},{0xC8,0x52},{0xC9,0x4B},{0xCB,0x50},{0xCD,0x4F},
        {0xCF,0x4D},{0xD0,0x51},{0xD1,0x4E},{0xD2,0x49},{0xD3,0x4C},{0xDD,0x65},
    };
    memset(g_dik2hid, 0, sizeof g_dik2hid);
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++)
        g_dik2hid[pairs[i][0]] = pairs[i][1];
}

static void ensure_table(void)
{
    if (g_tableReady) return;
    if (g_exeMatch) {
        /* Prefer the game's own table: it honours the "Keyboard Type" option. */
        const uint8_t *t = (const uint8_t *)TOD_DIK2HID_TABLE;
        if (t[0x1E * 4] == 0x04 && t[0x1C * 4] == 0x28 && t[0x39 * 4] == 0x2C) {
            for (int d = 0; d < 256; d++) g_dik2hid[d] = t[d * 4];
            g_tableReady = 1;
            logf_("using game's DIK->HID table");
            return;
        }
    }
    build_fallback_table();
    g_tableReady = 2; /* fallback; re-check game table later */
}

static int is_modifier_dik(int d)
{
    return d == 0x1D || d == 0x2A || d == 0x36 || d == 0x38 ||
           d == 0x9D || d == 0xB8 || d == 0xDB || d == 0xDC;
}

/* ---- Player 2 -> Maple port B ------------------------------------------- */
static void write_port_b(const volatile uint8_t *state /* NULL = release all */)
{
    if (!g_exeMatch) return;
    if (g_tableReady != 1) { g_tableReady = 0; ensure_table(); }

    uint8_t cond[8] = {0};
    uint8_t keys[6] = {0};
    int n = 0, overflow = 0;

    if (state) {
        /* Modifier bits exactly as the game builds port A (0x4B7AC6). */
        if (state[0x2A] & 0x80) cond[0] |= 0x02;  /* LShift */
        if (state[0x36] & 0x80) cond[0] |= 0x20;  /* RShift */
        if (state[0x38] & 0x80) cond[0] |= 0x40;  /* LAlt   */
        if (state[0xB8] & 0x80) cond[0] |= 0x04;  /* RAlt   */
        if (state[0x1D] & 0x80) cond[0] |= 0x01;  /* LCtrl  */
        if (state[0x9D] & 0x80) cond[0] |= 0x10;  /* RCtrl  */

        uint8_t held[256]; int nh = 0;
        for (int d = 0; d < 256; d++) {
            if (!(state[d] & 0x80) || is_modifier_dik(d) || d == 0x94) continue;
            uint8_t h = g_dik2hid[d];
            if (h > 0x03) held[nh++] = h;
        }
        /* keep previously reported keys in their slots, then add new ones */
        for (int i = 0; i < 6 && n < 6; i++) {
            if (!g_p2keys[i]) continue;
            for (int j = 0; j < nh; j++)
                if (held[j] == g_p2keys[i]) { keys[n++] = held[j]; held[j] = 0; break; }
        }
        for (int j = 0; j < nh; j++) {
            if (!held[j]) continue;
            if (n < 6) keys[n++] = held[j]; else overflow = 1;
        }
    }

    if (overflow) memset(&cond[2], 0x01, 6);   /* ErrorRollOver, like port A */
    else          memcpy(&cond[2], keys, 6);
    memcpy(g_p2keys, overflow ? g_p2keys : keys, 6);

    memcpy((void *)TOD_PORTB_COND, cond, 8);
}

/* ---- hooked IDirectInputDevice methods ---------------------------------- */
static HRESULT WINAPI hk_GetDeviceState(void *self, DWORD cb, void *data)
{
    HRESULT hr = o_GetDeviceState(self, cb, data);
    if (self != g_kbd) return hr;

    Tod2pShared *s = shm_live();
    if (!s) return hr;
    s->dllPolls++;
    if (SUCCEEDED(hr) && cb == 256 && data) {
        s->dllFlags |= TOD2P_DLL_GAME_FOCUS;
        for (int i = 0; i < 256; i++) ((uint8_t *)data)[i] = s->p1state[i];
        write_port_b(s->p2state);
    } else {
        s->dllFlags &= ~TOD2P_DLL_GAME_FOCUS;
        write_port_b(NULL);
    }
    return hr;
}

static HRESULT WINAPI hk_GetDeviceData(void *self, DWORD cb, void *rgdod,
                                       DWORD *pdwInOut, DWORD flags)
{
    DWORD cap = pdwInOut ? *pdwInOut : 0;
    HRESULT hr = o_GetDeviceData(self, cb, rgdod, pdwInOut, flags);
    if (self != g_kbd || !pdwInOut) return hr;

    Tod2pShared *s = shm_live();
    if (!s) return hr;

    uint32_t w = s->p1Write;
    MemoryBarrier();
    if (FAILED(hr)) {                 /* not acquired / no focus: drop queue */
        g_p1Read = w;
        return hr;
    }

    HRESULT ret = DI_OK_;
    uint32_t avail = w - g_p1Read;
    if (avail > TOD2P_RING) {
        g_p1Read = w - TOD2P_RING;
        avail = TOD2P_RING;
        ret = DI_BUFFEROVERFLOW_;
    }

    if (!rgdod) {                     /* count / flush request */
        if (!(flags & DIGDD_PEEK)) g_p1Read = w;
        *pdwInOut = avail;
        return ret;
    }

    uint32_t n = avail < cap ? avail : cap;
    DWORD stride = cb ? cb : sizeof(DIOBJDATA7);
    for (uint32_t i = 0; i < n; i++) {
        const Tod2pKeyEvent *e = &s->p1ring[(g_p1Read + i) & (TOD2P_RING - 1)];
        DIOBJDATA7 *o = (DIOBJDATA7 *)((uint8_t *)rgdod + i * stride);
        memset(o, 0, stride < sizeof *o ? stride : sizeof *o);
        o->dwOfs = e->dik;
        o->dwData = e->down ? 0x80 : 0x00;
        if (stride >= 12) o->dwTimeStamp = e->time;
        if (stride >= 16) o->dwSequence = ++g_seq;
    }
    if (!(flags & DIGDD_PEEK)) g_p1Read += n;
    *pdwInOut = n;
    return ret;
}

static void hook_keyboard(void *dev)
{
    void *p;
    g_kbd = dev;
    if ((p = patch_slot(dev, DID_SLOT_GETDEVICESTATE, (void *)hk_GetDeviceState)))
        o_GetDeviceState = (PFN_GetDeviceState)p;
    if ((p = patch_slot(dev, DID_SLOT_GETDEVICEDATA, (void *)hk_GetDeviceData)))
        o_GetDeviceData = (PFN_GetDeviceData)p;
    logf_("system keyboard device %p hooked", dev);
}

/* ---- hooked IDirectInput methods ---------------------------------------- */
static HRESULT WINAPI hk_CreateDevice(void *self, REFGUID g, void **out, void *outer)
{
    HRESULT hr = o_CreateDevice(self, g, out, outer);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(g, &GUID_SysKeyboard_))
        hook_keyboard(*out);
    return hr;
}

static HRESULT WINAPI hk_CreateDeviceEx(void *self, REFGUID g, REFIID riid, void **out, void *outer)
{
    HRESULT hr = o_CreateDeviceEx(self, g, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(g, &GUID_SysKeyboard_))
        hook_keyboard(*out);
    return hr;
}

static void hook_dinput(void *di, int isV7)
{
    void *p;
    if ((p = patch_slot(di, DI_SLOT_CREATEDEVICE, (void *)hk_CreateDevice)))
        o_CreateDevice = (PFN_CreateDevice)p;
    if (isV7 && (p = patch_slot(di, DI7_SLOT_CREATEDEVICEEX, (void *)hk_CreateDeviceEx)))
        o_CreateDeviceEx = (PFN_CreateDeviceEx)p;
}

/* ---- exports ------------------------------------------------------------ */
HRESULT WINAPI DirectInputCreateEx(HINSTANCE h, DWORD ver, REFIID riid, LPVOID *out, void * outer)
{
    if (!load_real() || !p_DirectInputCreateEx) return E_FAIL;
    HRESULT hr = p_DirectInputCreateEx(h, ver, riid, out, outer);
    logf_("DirectInputCreateEx(ver=0x%lx) -> 0x%08lx", (unsigned long)ver, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out)
        hook_dinput(*out, IsEqualGUID(riid, &IID_IDirectInput7A_) || IsEqualGUID(riid, &IID_IDirectInput7W_));
    return hr;
}

HRESULT WINAPI DirectInputCreateA(HINSTANCE h, DWORD ver, LPVOID *out, void * outer)
{
    if (!load_real() || !p_DirectInputCreateA) return E_FAIL;
    HRESULT hr = p_DirectInputCreateA(h, ver, out, outer);
    if (SUCCEEDED(hr) && out && *out) hook_dinput(*out, 0);
    return hr;
}

HRESULT WINAPI DirectInputCreateW(HINSTANCE h, DWORD ver, LPVOID *out, void * outer)
{
    if (!load_real() || !p_DirectInputCreateW) return E_FAIL;
    HRESULT hr = p_DirectInputCreateW(h, ver, out, outer);
    if (SUCCEEDED(hr) && out && *out) hook_dinput(*out, 0);
    return hr;
}

HRESULT WINAPI DllCanUnloadNow(void)
{ return (load_real() && p_DllCanUnloadNow) ? p_DllCanUnloadNow() : S_FALSE; }
HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, LPVOID *o)
{ return (load_real() && p_DllGetClassObject) ? p_DllGetClassObject(c, r, o) : CLASS_E_CLASSNOTAVAILABLE; }
HRESULT WINAPI DllRegisterServer(void)
{ return (load_real() && p_DllRegisterServer) ? p_DllRegisterServer() : E_FAIL; }
HRESULT WINAPI DllUnregisterServer(void)
{ return (load_real() && p_DllUnregisterServer) ? p_DllUnregisterServer() : E_FAIL; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        open_log();
        const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
        const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
        g_exeMatch = (dos->e_magic == IMAGE_DOS_SIGNATURE &&
                      nt->Signature == IMAGE_NT_SIGNATURE &&
                      nt->FileHeader.TimeDateStamp == TOD_EXE_TIMESTAMP &&
                      nt->OptionalHeader.ImageBase == 0x400000);
        logf_("TotD2P proxy loaded; exe timestamp 0x%08lx -> %s",
              (unsigned long)nt->FileHeader.TimeDateStamp,
              g_exeMatch ? "known build, P2 enabled" : "UNKNOWN build, P2 disabled");

        /* display mode: must be applied now, before the game's WinMain runs */
        g_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, TOD2P_SHM_NAME);
        if (g_map) {
            g_shm = (Tod2pShared *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Tod2pShared));
            if (g_shm && g_shm->magic == TOD2P_MAGIC && g_shm->version == TOD2P_VERSION) {
                int live = g_shm->active && (GetTickCount() - g_shm->heartbeat) < TOD2P_STALE_MS;
                if (live && g_exeMatch && g_shm->displayMode) {
                    tod2p_display_init((int)g_shm->displayMode);
                    g_shm->dllFlags |= TOD2P_DLL_DISPLAY;
                }
            } else {
                if (g_shm) UnmapViewOfFile(g_shm);
                CloseHandle(g_map);
                g_shm = NULL; g_map = NULL;
                logf_("launcher shared memory has a different version; ignoring");
            }
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_shm) { g_shm->dllFlags = 0; UnmapViewOfFile(g_shm); }
        if (g_map) CloseHandle(g_map);
        if (g_log) fclose(g_log);
    }
    return TRUE;
}

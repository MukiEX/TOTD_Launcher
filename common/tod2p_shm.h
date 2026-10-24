/*
 * TotD2P - shared memory contract between the launcher (TotD2P.exe) and the
 * proxy dinput.dll loaded into Tod_e.exe.
 *
 * The launcher owns Raw Input (so we never fight DirectInput inside the game
 * process for the keyboard raw-input registration). It publishes:
 *   - p1state / p2state : 256-byte DIK-indexed key state (0x80 = down)
 *   - p1ring            : ordered P1 key transitions (feeds GetDeviceData)
 * The DLL rebuilds the game's keyboard view from P1 data and writes P2's
 * Dreamcast-style keyboard condition into the game's port-B buffer.
 *
 * Single writer (launcher), single reader (DLL). Lock-free: write the ring
 * slot, barrier, then bump p1Write.
 */
#ifndef TOD2P_SHM_H
#define TOD2P_SHM_H

#include <stdint.h>

#define TOD2P_SHM_NAME     L"Local\\TotD2P_SharedState_v1"
#define TOD2P_MAGIC        0x50325444u /* 'DT2P' */
#define TOD2P_VERSION      2u
#define TOD2P_RING         512u        /* power of two */
#define TOD2P_STALE_MS     3000u       /* launcher heartbeat timeout */

typedef struct {
    uint8_t  dik;      /* DirectInput key code (scan code | 0x80 for E0) */
    uint8_t  down;     /* 1 = pressed, 0 = released */
    uint16_t pad;
    uint32_t time;     /* GetMessageTime() of the raw event */
} Tod2pKeyEvent;

typedef struct {
    uint32_t magic;
    uint32_t version;

    volatile uint32_t heartbeat;   /* launcher GetTickCount(), every ~250 ms */
    volatile uint32_t active;      /* 1 = launcher wants the DLL to take over */

    volatile uint8_t  p1state[256];
    volatile uint8_t  p2state[256];

    volatile uint32_t p1Write;     /* total P1 events ever written */
    Tod2pKeyEvent     p1ring[TOD2P_RING];

    /* DLL -> launcher status (informational, shown in launcher UI) */
    volatile uint32_t dllHeartbeat;  /* GetTickCount() at last hooked poll */
    volatile uint32_t dllFlags;      /* TOD2P_DLL_* */
    volatile uint32_t dllPolls;      /* hooked keyboard polls seen */

    /* launcher -> DLL, read once when the game starts */
    volatile uint32_t displayMode;   /* TOD2P_DISPLAY_* */
} Tod2pShared;

#define TOD2P_DISPLAY_ORIGINAL    0u  /* stock 640x480 fullscreen            */
#define TOD2P_DISPLAY_FULL_NATIVE 1u  /* exclusive fullscreen, desktop res   */
#define TOD2P_DISPLAY_BORDERLESS  2u  /* 4:3 at desktop height, black bars   */
#define TOD2P_DISPLAY_WINDOW2X    3u  /* 1280x960 window                     */
#define TOD2P_DISPLAY_COUNT       4u

#define TOD2P_DLL_LOADED      0x01u  /* proxy loaded into a process */
#define TOD2P_DLL_KBD_HOOKED  0x02u  /* keyboard device hooked */
#define TOD2P_DLL_EXE_MATCH   0x04u  /* known Tod_e.exe build: P2 port writes on */
#define TOD2P_DLL_GAME_FOCUS  0x08u  /* last poll succeeded (game has focus) */
#define TOD2P_DLL_DISPLAY     0x10u  /* display mode patches applied */

#endif

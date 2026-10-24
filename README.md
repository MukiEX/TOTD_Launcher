# TotD2P — two keyboards for The Typing of the Dead (PC)

Local 2-player co-op on one PC with two keyboards, for the Windows release of
*The Typing of the Dead* (`Tod_e.exe`, build dated 2000-11-23).

## Install

1. Copy `dinput.dll` into the game folder, next to `Tod_e.exe`.
2. Copy `TotD2P.exe` anywhere (the game folder is easiest).
3. Run `TotD2P.exe`. If it isn't in the game folder, press **F2** once to
   locate `Tod_e.exe` (remembered in `tod2p.ini`).

## Use

- Press any key on each keyboard: it appears as a card under **Player 1**.
- On the keyboard you want for player 2, press **→** or **D**: its card moves to
  **Player 2**. **←** / **A** moves it back. Unlisted keyboards are Player 1.
- Press **Enter** to start the game. **Leave the launcher open** while you play;
  closing it turns 2-keyboard mode off (the game falls back to stock behaviour).
- Launcher keys only work while the launcher window is in front, so typing
  in-game never reassigns keyboards.

### Display mode (F3)

Press **F3** in the launcher to cycle display modes. The choice is saved and
applies the next time the game starts:

| Mode | What you get |
|---|---|
| Original 640x480 | Stock behaviour |
| Fullscreen native (stretched) | Exclusive fullscreen at your desktop resolution; 4:3 image stretched to fill |
| Borderless 4:3 | Rendered at your desktop height in 4:3 (e.g. 1440x1080 on 1080p), centred with black bars |
| Window 1280x960 | A normal 2x window |

The engine (Sega's HGL layer) already supports any resolution. The game just
hardcodes 640x480. The proxy patches that mode table entry, plus the
hardcoded 640x480 used at window creation, before the game starts, so the
D3D back buffer is genuinely rendered at the new size. Windowed modes ignore
`FullScreen=T` in `hglCore.ini`.

Assignments are saved per keyboard (by USB device path), so the same keyboard on
the same USB port keeps its slot. If you move a keyboard to another port, press
A/D on it once again.

## How it works

The PC port emulates the Dreamcast's Maple controller ports. Ports A and B are
both hard-wired as keyboards, and the game logic gives a player slot to every
port reporting a keyboard. The PC input layer fills port A from DirectInput but
deliberately skips port B, which only the (still present) DirectPlay network mode
writes to: the remote player's keyboard goes into port B's 8-byte Dreamcast
keyboard block at `0x847F10`.

- **TotD2P.exe** reads all keyboards with Raw Input (one stream per physical
  device) and publishes Player 1 / Player 2 key state through shared memory.
- **dinput.dll** (proxy) forwards everything to the real `dinput.dll` and hooks
  the system keyboard device. The game's keyboard reads (`GetDeviceData` +
  `GetDeviceState`) now return Player 1's keyboards only, and Player 2's keys are
  written to port B in the same poll, in the game's own format (modifier bits as
  the game builds them, HID key codes through the game's DIK→HID table, so the
  Keyboard Type option applies, plus 6-key rollover with ErrorRollOver).

Safety: the DLL only writes port B when the exe's PE timestamp is `0x3A1CF9B4`
and it loads at `0x400000`. Any other build gets P1 filtering only. Without a
running launcher the DLL is a pure passthrough.

`tod2p.log` in the game folder records what the proxy did.

## Wine / Proton (Linux, Steam Deck)

Wine uses its built-in `dinput` unless told otherwise. Set
`WINEDLLOVERRIDES="dinput=n,b"` (or add a `dinput` *native, builtin* override in
winecfg). Run `TotD2P.exe` in the same prefix, since shared memory is per prefix.
Under Wine, Raw Input may not tell physical keyboards apart, depending on the
Wine version.

## Status

Tested under Wine with a harness that impersonates `Tod_e.exe` (same PE
timestamp, memory at the game's addresses): P1 filtering, P2 port-B output, key
slot ordering, rollover, launcher-loss release, buffer overflow, and real Raw
Input → launcher → proxy for both players. **Not yet tested in the real game.**
Display modes are tested only for the patching and window placement (harness
under Wine), not for in-game rendering. Open questions:

- Whether every 2D element scales with resolution (strong evidence says yes:
  the game positions things in a fixed 640x480 virtual space and the engine
  already handles a 320x240 mode), and whether windowed modes keep the right
  game speed.

- How player 2 joins in-game (Enter/Start-style key, or any typing).
- Whether name entry or menus read typed characters from window messages. If so,
  P2's keys would also reach those screens as P1.

## Build

Needs `i686-w64-mingw32-gcc`. Run `make`, which writes `build/dinput.dll` and
`build/TotD2P.exe`. `test/fakegame.c` is the test harness.

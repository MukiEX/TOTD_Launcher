# The Typing of the Dead — Two-keyboard Launcher

<img width="884" height="552" alt="image" src="https://github.com/user-attachments/assets/9dfb395d-afd8-4bd4-a3ee-22de2298dbdd" />

Local two-player keyboard input and modern graphics support for the Windows
release of The Typing of the Dead (`Tod_e.exe`, timestamp `0x3A1CF9B4`).

## Features

- Assign physical keyboards independently to Player 1 or Player 2.
- Retain keyboard assignments between sessions.
- Embed and deploy the DirectInput proxy and dgVoodoo DirectDraw components.
- Select original 640x480, native fullscreen, borderless 4:3, or 1280x960 window modes.
- Start the game without requesting administrator privileges.
- Convert Player 2 printable keys into the character format expected by gameplay.

## Build on Windows

Install an LLVM-MinGW or MinGW-w64 toolchain with an i686 Windows target.
The development build used LLVM-MinGW 20261020 UCRT.

From this repository:

```powershell
.\scripts\fetch-dgvoodoo.ps1
.\build-windows.ps1 -ToolchainBin 'C:\path\to\llvm-mingw\bin'
```

This produces `build\TotD2P.exe` and `build\dinput.dll`. The launcher embeds
all required proxy DLLs and a default graphics configuration, so only the
launcher needs to be copied to a writable game folder.

With GNU make, run the fetch script first, then `make` using an
`i686-w64-mingw32-gcc` / `i686-w64-mingw32-windres` toolchain.

## Use

1. Close previous game and launcher instances. Put the built `TotD2P.exe`
   beside `Tod_e.exe`, then run it normally. F2 can select a game elsewhere.
2. Press a key on each keyboard to display it.
3. Press Right/D on the second keyboard to assign it to Player 2.
   Left/A assigns a keyboard to Player 1.
4. Press F3 to choose a display mode, then Enter to start.
5. Keep the launcher open while playing.

Launcher shortcuts apply only while its window is focused. Unassigned
keyboards belong to Player 1. The launcher writes settings to `tod2p.ini`
and the proxy writes diagnostics to `tod2p.log` beside the game.

At launch, the embedded DLLs are extracted beside the game. A different
existing DLL is preserved as `NAME.pre-dgvoodoo`; deployment stops if that
backup already exists. Resolve an existing backup before another conflicting
update. An existing `dgVoodoo.conf` is preserved except for the display
settings owned by F3. No game executable is distributed or modified.

## Graphics

The game keeps its original 640x480 display initialization. dgVoodoo controls
higher-resolution rendering and output, avoiding HGL's exact-mode device
checks. The default backend is D3D12; change `OutputAPI` in `dgVoodoo.conf`
to `d3d11_fl11_0` if needed. Old Windows compatibility settings can interfere.
The launcher does not change those settings or write system registry keys.

The launcher has an `asInvoker` manifest and starts the game with
`RunAsInvoker` in its child environment. A writable installation folder is
still required.

## Verification and status

The Windows build passes compilation with warnings enabled.
`test/integration.c` checks deployment, backups, configuration preservation,
and all four display settings. `test/letters.c` checks printable conversion,
Shift, controls, port-B output, release, and rollover. See
[tests](test/README.md) for commands.

The user confirmed Player 2 Enter/start input worked. The printable-letter
fix passes the isolated tests but still needs verification with two physical
keyboards during gameplay. Full game launching could not be tested in the
agent environment due to a Windows DLL relocation error.

`test/fakegame.c` is retained from the original source. Its direct engine
resolution-patching tests describe the old implementation and no longer apply.

## Credits and third-party components

- Original launcher and DirectInput proxy: **Claude**, as identified by the
  project owner. The original source is the first commit in this repository.
- dgVoodoo integration, wrapper-based display handling, printable Player 2
  conversion, and packaging: **OpenAI Codex**, at the project owner's request.
- dgVoodoo 2.87.5: **Dege**, distributed separately from its
  [official releases](https://github.com/dege-diosg/dgVoodoo2/releases/tag/v2.87.5).

See [AUTHORS.md](AUTHORS.md) and [LICENSE-NOTICE.md](LICENSE-NOTICE.md).

# TotD2P with dgVoodoo 2.87.5

## Player 2 letters fix (v3)

The original P2 proxy sent raw HID codes. The game converts printable P1
keys to ASCII with bit 7 set (routine 0x4B8050), and gameplay expects that
converted format. Enter worked because control keys retain their HID code.
The proxy now converts P2 printable keys using its own held modifier state
and the current Windows keyboard layout, retaining HID codes for controls.
The game's Japanese Shift+3 special case is also retained. test/letters.c
passed 22 assertions covering letters, Shift, Enter, Space, arrow keys,
actual port-B output, release, and rollover. Two physical keyboards in the
real game still require user verification.

Run TotD2P.exe normally, assign a second keyboard with Right/D, select a
display mode with F3, then press Enter. Leave the launcher open while playing.
The existing four display modes and two-keyboard routing are retained.

The launcher embeds the TotD2P dinput proxy, dgVoodoo's unmodified x86
DDraw.dll and D3DImm.dll, and a default dgVoodoo.conf. It deploys them next
to the selected game before launch. An existing config is preserved.
Different existing DLLs are copied to NAME.pre-dgvoodoo before replacement.
If that backup already exists, deployment stops instead of overwriting it.
Close the game before deploying an update. Use a writable game folder.

The launcher has an asInvoker manifest and starts the game with the child
compatibility environment RunAsInvoker. This avoids requesting elevation;
it does not grant access to protected files or registry keys. Existing
Windows compatibility settings can still affect the game. No registry
changes are made by this build. The original game executable is unmodified.

The default dgVoodoo config uses D3D12 and no watermark. F3 now writes
dgVoodoo's DirectX Resolution, AppControlledScreenMode, General FullScreenMode,
ScalingMode, CenterAppWindow, and GeneralExt WindowedAttributes before launch.
The game keeps its original 640x480 mode table and window initialization.
dgVoodoo handles the higher render resolution and screen presentation. This
replaces the old engine display patch, which failed hardware enumeration for
non-original modes on the user's desktop. Other config preferences are preserved.
For a machine without D3D12 support, edit OutputAPI
in dgVoodoo.conf to d3d11_fl11_0.

Build with MinGW using make, or build-windows.ps1 -ToolchainBin PATH.
Built here with LLVM-MinGW 20261020 (i686 UCRT target). test/integration.c
tests extraction, repeat deployment, backup creation, backup collision,
and preserving a custom config, plus settings for all four display modes;
all 20 assertions passed in an x64 host build. The original fakegame harness
remains as a historical input test; its direct engine-display patch tests no
longer apply to this version.

Live game launch could not be verified: Windows returned Illegal System DLL
Relocation before game startup in this execution environment, including for
a renamed byte-identical game executable. Two physical keyboards and actual
high-resolution rendering still need validation on the desktop.

dgVoodoo is by Dege and remains a separate, closed-source graphics component.
Source and original distribution: https://github.com/dege-diosg/dgVoodoo2/releases/tag/v2.87.5
The original dgVoodoo archive is included alongside this package.

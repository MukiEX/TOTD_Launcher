param([Parameter(Mandatory=$true)][string]$ToolchainBin)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force build | Out-Null
    & "$ToolchainBin\i686-w64-mingw32-gcc.exe" -O2 -s -Wall -Wextra -Wno-missing-field-initializers -shared -o build/dinput.dll proxy/dinput_proxy.c proxy/display.c proxy/dinput.def '-Wl,--kill-at,--enable-stdcall-fixup' -lgdi32
    if ($LASTEXITCODE) { throw 'Proxy compilation failed' }
    & "$ToolchainBin\i686-w64-mingw32-windres.exe" -I . launcher/resources.rc -O coff -o build/resources.o
    if ($LASTEXITCODE) { throw 'Resource compilation failed' }
    & "$ToolchainBin\i686-w64-mingw32-gcc.exe" -O2 -s -Wall -Wextra -Wno-missing-field-initializers -municode -mwindows -o build/TotD2P.exe launcher/launcher.c build/resources.o -lhid -lcomdlg32 -lgdi32
    if ($LASTEXITCODE) { throw 'Launcher compilation failed' }
} finally { Pop-Location }

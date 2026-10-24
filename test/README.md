# Native Windows checks

Build the project first. The following tests use an x64 LLVM-MinGW host to
exercise deployment and keyboard conversion independently of the 32-bit game:

```powershell
$bin = 'C:\path\to\llvm-mingw\bin'
& "$bin\x86_64-w64-mingw32-windres.exe" -I . launcher/resources.rc -O coff -o build/resources64.o
& "$bin\x86_64-w64-mingw32-gcc.exe" -O2 -Wall -Wextra -Wno-missing-field-initializers -o build/integration-test.exe test/integration.c build/resources64.o -lhid -lcomdlg32 -lgdi32
& .\build\integration-test.exe
& "$bin\x86_64-w64-mingw32-gcc.exe" -O2 -Wall -Wextra -Wno-missing-field-initializers -o build/letters-test.exe test/letters.c -lgdi32
& .\build\letters-test.exe
```

Run these commands from a clean repository root. Deployment tests create
`integration-scratch`; remove that test directory before repeating them.
The letters test temporarily selects the US layout for its thread, then
restores the previous layout. Its fake port-B memory is process-local.

Expected: 20 deployment/configuration assertions and 22 letter/port assertions
pass. These tests do not replace real gameplay validation with two keyboards.

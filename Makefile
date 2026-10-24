CC      = i686-w64-mingw32-gcc
CFLAGS  = -O2 -s -Wall -Wextra -Wno-missing-field-initializers -static-libgcc
OUT     = build
WINDRES = i686-w64-mingw32-windres

all: $(OUT)/dinput.dll $(OUT)/TotD2P.exe

$(OUT)/dinput.dll: proxy/dinput_proxy.c proxy/display.c proxy/dinput.def common/tod2p_shm.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ proxy/dinput_proxy.c proxy/display.c proxy/dinput.def -Wl,--kill-at,--enable-stdcall-fixup -lgdi32

$(OUT)/resources.o: launcher/resources.rc launcher/launcher.manifest $(OUT)/dinput.dll vendor/dgVoodoo/DDraw.dll vendor/dgVoodoo/D3DImm.dll vendor/dgVoodoo/dgVoodoo.conf
	$(WINDRES) -I . launcher/resources.rc -O coff -o $@

$(OUT)/TotD2P.exe: launcher/launcher.c common/tod2p_shm.h $(OUT)/resources.o
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -municode -mwindows -o $@ launcher/launcher.c $(OUT)/resources.o -lhid -lcomdlg32 -lgdi32

clean:
	rm -rf $(OUT)

/* Native x64 test for the resource deployment code (no game or input needed). */
#include "../launcher/launcher.c"
static int failures;
#define CHECK(x) do { printf("%s: %s\n", (x) ? "PASS" : (failures++, "FAIL"), #x); } while (0)
static void write_test(const WCHAR *name, const char *value) {
    HANDLE h = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD n; WriteFile(h, value, (DWORD)strlen(value), &n, NULL); CloseHandle(h);
}
int main(void) {
    WCHAR dir[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, dir);
    wcscat(dir, L"\\integration-scratch\\"); CreateDirectoryW(dir, NULL);
    CHECK(prepare_graphics(dir));
    CHECK(prepare_graphics(dir)); /* identical resources may be deployed again */
    WCHAR dll[MAX_PATH], backup[MAX_PATH], config[MAX_PATH];
    _snwprintf(dll, MAX_PATH, L"%lsDDraw.dll", dir);
    _snwprintf(backup, MAX_PATH, L"%ls.pre-dgvoodoo", dll);
    _snwprintf(config, MAX_PATH, L"%lsdgVoodoo.conf", dir);
    write_test(dll, "previous wrapper");
    CHECK(prepare_graphics(dir)); CHECK(file_exists(backup));
    write_test(dll, "another wrapper");
    CHECK(!prepare_graphics(dir)); /* never replace a previous backup */
    write_test(config, "custom settings");
    CHECK(unpack_file(dir, L"dgVoodoo.conf", 203, 1));
    CHECK(GetFileAttributesW(config) != INVALID_FILE_ATTRIBUTES);
    HANDLE h = CreateFileW(config, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(GetFileSize(h, NULL) == strlen("custom settings")); CloseHandle(h);
    for (uint32_t mode = 0; mode < TOD2P_DISPLAY_COUNT; mode++) {
        CHECK(configure_display(dir, mode));
        WCHAR value[64];
        GetPrivateProfileStringW(L"General", L"FullScreenMode", L"", value, 64, config);
        CHECK(!wcscmp(value, mode >= 2 ? L"false" : L"true"));
        GetPrivateProfileStringW(L"DirectX", L"Resolution", L"", value, 64, config);
        CHECK(mode == 0 ? !wcscmp(value, L"unforced") : wcsstr(value, L"h:") != NULL);
    }
    return failures ? 1 : 0;
}

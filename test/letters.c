/* Exercise the actual P2 gameplay conversion without running the game. */
#include "../proxy/dinput_proxy.c"
void tod2p_display_init(int mode) { (void)mode; }
static int failures;
#define CHECK(x) do { printf("%s: %s\n", (x) ? "PASS" : (failures++, "FAIL"), #x); } while (0)
int main(void) {
    HKL old = GetKeyboardLayout(0);
    HKL us = LoadKeyboardLayoutW(L"00000409", KLF_ACTIVATE);
    CHECK(us != NULL);
    uint8_t state[256] = {0};
    state[0x1e] = 0x80;
    uint8_t a = gameplay_key(0x1e, 0x04, state);
    CHECK((a & 0xdf) == ('A' | 0x80));
    state[0x2a] = 0x80;
    CHECK(gameplay_key(0x1e, 0x04, state) == (a ^ 0x20));
    state[0x2a] = 0;
    for (int d = 0x10; d <= 0x19; d++) {
        static const char letters[] = "QWERTYUIOP";
        memset(state, 0, sizeof state); state[d] = 0x80;
        CHECK((gameplay_key(d, 0, state) & 0xdf) == (letters[d - 0x10] | 0x80));
    }
    memset(state, 0, sizeof state); state[0x1c] = 0x80;
    CHECK(gameplay_key(0x1c, 0x28, state) == 0x28);
    memset(state, 0, sizeof state); state[0x39] = 0x80;
    CHECK(gameplay_key(0x39, 0x2c, state) == 0xa0);
    memset(state, 0, sizeof state); state[0xcb] = 0x80;
    CHECK(gameplay_key(0xcb, 0x50, state) == 0x50);
    void *game = VirtualAlloc((void *)0x00840000u, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(game == (void *)0x00840000u);
    if (game) {
        g_exeMatch = 1;
        memset(state, 0, sizeof state); state[0x1e] = 0x80;
        write_port_b(state);
        uint8_t *port = (uint8_t *)TOD_PORTB_COND;
        CHECK(port[2] == a);
        state[0x2a] = 0x80;
        write_port_b(state);
        CHECK(port[0] == 2 && port[2] == (a ^ 0x20));
        state[0x2a] = 0; state[0x1c] = 0x80;
        write_port_b(state);
        CHECK(port[2] == a && port[3] == 0x28);
        write_port_b(NULL);
        CHECK(port[0] == 0 && port[2] == 0 && port[3] == 0);
        memset(state, 0, sizeof state);
        for (int d = 0x10; d <= 0x16; d++) state[d] = 0x80;
        write_port_b(state);
        CHECK(port[2] == 1 && port[7] == 1);
        VirtualFree(game, 0, MEM_RELEASE);
    }
    ActivateKeyboardLayout(old, 0);
    return failures ? 1 : 0;
}

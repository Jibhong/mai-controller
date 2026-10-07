/*
 * majdataplayio.c  -  MaiController -> MajdataPlay (Pipe backend) bridge, Windows
 *
 *   MaiController (USB CDC, VID 2E8A / PID 000A, 16-byte packets)
 *        |  auto-scan by VID/PID, auto-reconnect
 *        v
 *   this program  (named-pipe SERVER, replaces IO2Pipe)
 *        |-- \\.\pipe\MajdataPlay.IO.ButtonRing.{N}P   (BA1..BA8, TEST, SERVICE)
 *        |-- \\.\pipe\MajdataPlay.IO.TouchPanel.{N}P   (A1..E8, 34 zones)
 *        v
 *   MajdataPlay (named-pipe CLIENT, IO Manufacturer = "Pipe")
 *
 * Frame (both pipes): 48 04 | type(1) | version LE16 = 0 | length LE16 | payload
 *   type 1 = Report, payload = 8-byte little-endian UInt64 state.
 *   Touch bits : 0-7 A1-A8, 8-15 B1-B8, 16-17 C1-C2, 18-25 D1-D8, 26-33 E1-E8
 *   Button bits: 0-7 BA1-BA8, 8 TEST, 9 P1, 10 SERVICE, 11 P2
 * Protocol source: https://github.com/TeamMajdata/IO2Pipe
 *
 * Build (MinGW):  gcc -O2 -o majdataplayio.exe majdataplayio.c -lsetupapi -ladvapi32
 * Usage:          majdataplayio.exe [--player 1|2] [--vid 2E8A] [--pid 000A] [--touch-to-buttons]
 *   --touch-to-buttons  (-t)  also press button N while touch zone AN is touched (A1-A8 -> BA1-BA8)
 * Do not run together with IO2Pipe (same pipe names).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

/* ------------------------------------------------------------------ config */

#define DEFAULT_VID      0x2E8A
#define DEFAULT_PID      0x000A
#define DEVICE_BAUD      115200     /* ignored by native USB CDC, set anyway */
#define RESCAN_MS        500        /* device rescan interval                 */
#define WATCHDOG_MS      2000       /* no valid packet this long -> reconnect */
#define SNAPSHOT_MS      500        /* resend latest state while idle         */

/* ------------------------------------------------------ device packet parse */

#define PKT_SIZE 16
#define SYNC0    0xAA
#define SYNC1    0x55

typedef struct {
    uint8_t touch[7];   /* 34 zones, 5 per byte, zone z = byte z/5, bit z%5 */
    uint8_t btn;        /* bit n = game button n+1                          */
    uint8_t test, service, coin;
} State;

static uint8_t g_rx[512];
static size_t  g_rxn;

static bool parse_packets(State *st)
{
    bool got = false;
    size_t i = 0;

    while (g_rxn - i >= PKT_SIZE) {
        const uint8_t *p = &g_rx[i];
        if (p[0] != SYNC0 || p[1] != SYNC1) { i++; continue; }

        uint8_t x = 0;
        for (int j = 2; j <= 14; j++) x ^= p[j];
        if (x != p[15]) { i++; continue; }          /* bad checksum -> resync */

        memcpy(st->touch, &p[3], 7);
        st->btn     = p[10];
        st->test    = p[12];
        st->service = p[13];
        st->coin    = p[14];
        got = true;
        i += PKT_SIZE;
    }
    if (i) {
        memmove(g_rx, g_rx + i, g_rxn - i);
        g_rxn -= i;
    }
    return got;
}

static uint64_t touch_to_u64(const uint8_t t[7])
{
    uint64_t s = 0;
    for (int z = 0; z < 34; z++)
        if ((t[z / 5] >> (z % 5)) & 1u) s |= 1ULL << z;
    return s;
}

static uint64_t buttons_to_u64(const State *st)
{
    uint64_t b = st->btn;                 /* bits 0-7 = BA1-BA8 */
    if (st->test)    b |= 1ULL << 8;      /* TEST    */
    if (st->service) b |= 1ULL << 10;     /* SERVICE */
    return b;
}

/* ------------------------------------------------------------ device serial */

static HANDLE open_device(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    dcb.BaudRate     = DEVICE_BAUD;
    dcb.ByteSize     = 8;
    dcb.Parity       = NOPARITY;
    dcb.StopBits     = ONESTOPBIT;
    dcb.fBinary      = TRUE;
    dcb.fDtrControl  = DTR_CONTROL_ENABLE;   /* CDC firmware often waits for DTR */
    dcb.fRtsControl  = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = dcb.fInX = FALSE;
    SetCommState(h, &dcb);

    /* Return at once if bytes are waiting, otherwise wait up to 5 ms. */
    COMMTIMEOUTS to;
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout        = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant   = 5;
    to.WriteTotalTimeoutConstant  = 20;
    SetCommTimeouts(h, &to);

    SetupComm(h, 4096, 4096);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return h;
}

/* Find the COM port whose hardware ID contains VID_xxxx&PID_xxxx. */
static bool find_device_port(uint16_t vid, uint16_t pid, char *out, size_t outsz)
{
    char needle[32];
    snprintf(needle, sizeof needle, "VID_%04X&PID_%04X", vid, pid);

    HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;

    SP_DEVINFO_DATA d;
    d.cbSize = sizeof d;
    bool found = false;

    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &d); i++) {
        char hwid[1024];
        memset(hwid, 0, sizeof hwid);
        if (!SetupDiGetDeviceRegistryPropertyA(set, &d, SPDRP_HARDWAREID, NULL,
                                               (PBYTE)hwid, sizeof hwid - 2, NULL))
            continue;

        bool match = false;                           /* REG_MULTI_SZ */
        for (const char *s = hwid; *s; s += strlen(s) + 1) {
            char up[256];
            strncpy(up, s, sizeof up - 1);
            up[sizeof up - 1] = 0;
            _strupr(up);
            if (strstr(up, needle)) { match = true; break; }
        }
        if (!match) continue;

        HKEY key = SetupDiOpenDevRegKey(set, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == (HKEY)INVALID_HANDLE_VALUE) continue;

        char name[64];
        DWORD sz = sizeof name, type = 0;
        if (RegQueryValueExA(key, "PortName", NULL, &type, (LPBYTE)name, &sz) == ERROR_SUCCESS
            && type == REG_SZ) {
            snprintf(out, outsz, "\\\\.\\%s", name);
            found = true;
        }
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

/* ------------------------------------------------------------ pipe servers */

typedef struct {
    HANDLE      h;
    bool        connected;
    uint64_t    last;
    DWORD       last_send;
    const char *label;
} Pipe;

static bool pipe_create(Pipe *p, const char *base, int player, const char *label)
{
    char path[160];
    snprintf(path, sizeof path, "\\\\.\\pipe\\%s.%dP", base, player);
    memset(p, 0, sizeof *p);
    p->label = label;
    p->h = CreateNamedPipeA(
        path,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 8192, 8192, 0, NULL);
    if (p->h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[pipe] cannot create %s (error %lu) - is IO2Pipe or another copy running?\n",
                path, GetLastError());
        return false;
    }
    printf("[pipe] serving %s\n", path);
    return true;
}

static bool write_report(HANDLE h, uint64_t state)
{
    uint8_t f[15];
    f[0] = 0x48; f[1] = 0x04;            /* magic            */
    f[2] = 1;                            /* type: Report     */
    f[3] = 0;    f[4] = 0;               /* version = 0      */
    f[5] = 8;    f[6] = 0;               /* payload length 8 */
    for (int i = 0; i < 8; i++) f[7 + i] = (uint8_t)(state >> (8 * i));

    DWORD w = 0;
    return WriteFile(h, f, sizeof f, &w, NULL) && w == sizeof f;
}

static void pipe_service(Pipe *p, uint64_t state, DWORD now)
{
    bool force = false;

    if (!p->connected) {
        BOOL ok  = ConnectNamedPipe(p->h, NULL);
        DWORD er = ok ? 0 : GetLastError();
        if (ok || er == ERROR_PIPE_CONNECTED) {
            p->connected = true;
            force = true;                         /* send latest state at once */
            printf("[pipe] game connected to %s\n", p->label);
        } else {
            if (er == ERROR_NO_DATA) DisconnectNamedPipe(p->h);  /* old client left */
            return;                               /* ERROR_PIPE_LISTENING: wait */
        }
    }

    if (force || state != p->last || now - p->last_send >= SNAPSHOT_MS) {
        if (!write_report(p->h, state)) {
            DisconnectNamedPipe(p->h);
            p->connected = false;
            printf("[pipe] game left %s\n", p->label);
            return;
        }
        p->last = state;
        p->last_send = now;
    }
}

/* -------------------------------------------------------------------- main */

static volatile bool g_run = true;

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    g_run = false;
    return TRUE;
}

int main(int argc, char **argv)
{
    uint16_t vid = DEFAULT_VID, pid = DEFAULT_PID;
    int player = 1;
    bool touch_to_buttons = false;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--touch-to-buttons") || !strcmp(argv[i], "-t")) touch_to_buttons = true;
        else if (!strcmp(argv[i], "--vid")    && i + 1 < argc) vid    = (uint16_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--pid")    && i + 1 < argc) pid    = (uint16_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--player") && i + 1 < argc) player = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s [--player 1|2] [--vid hex] [--pid hex] [--touch-to-buttons]\n", argv[0]); return 1; }
    }
    if (player != 1 && player != 2) player = 1;

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    printf("majdataplayio: VID_%04X&PID_%04X -> player %d pipes%s\n", vid, pid, player,
           touch_to_buttons ? " (touch A1-A8 also pressed as buttons 1-8)" : "");

    Pipe buttons, touch;
    if (!pipe_create(&buttons, "MajdataPlay.IO.ButtonRing", player, "ButtonRing")) return 1;
    if (!pipe_create(&touch,   "MajdataPlay.IO.TouchPanel", player, "TouchPanel")) return 1;

    HANDLE dev = INVALID_HANDLE_VALUE;
    State  cur;
    memset(&cur, 0, sizeof cur);
    DWORD  last_dev_try = 0, last_pkt = 0;

    while (g_run) {
        DWORD now = GetTickCount();

        /* ---- device: scan by VID/PID and (re)connect ---- */
        if (dev == INVALID_HANDLE_VALUE && now - last_dev_try >= RESCAN_MS) {
            last_dev_try = now;
            char path[64];
            if (find_device_port(vid, pid, path, sizeof path)) {
                dev = open_device(path);
                if (dev != INVALID_HANDLE_VALUE) {
                    printf("[dev] connected on %s\n", path + 4);
                    g_rxn = 0;
                    last_pkt = now;
                }
            }
        }

        bool drop = false;
        if (dev != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            if (!ReadFile(dev, g_rx + g_rxn, (DWORD)(sizeof g_rx - g_rxn), &got, NULL)) {
                drop = true;                              /* unplugged / error */
            } else {
                g_rxn += got;
                if (parse_packets(&cur)) last_pkt = now;
                else if (g_rxn >= sizeof g_rx) g_rxn = 0; /* garbage flood guard */
                if (now - last_pkt > WATCHDOG_MS) drop = true;
            }
        } else {
            Sleep(5);
        }

        if (drop) {
            printf("[dev] lost, rescanning...\n");
            CloseHandle(dev);
            dev = INVALID_HANDLE_VALUE;
            g_rxn = 0;
            memset(&cur, 0, sizeof cur);                  /* release everything */
        }

        /* ---- publish to the game ---- */
        uint64_t touch_state = touch_to_u64(cur.touch);
        uint64_t btn_state   = buttons_to_u64(&cur);
        if (touch_to_buttons) btn_state |= touch_state & 0xFFULL;   /* A1-A8 -> BA1-BA8 */
        pipe_service(&buttons, btn_state, now);
        pipe_service(&touch,   touch_state, now);
    }

    /* shutdown: clear state */
    if (buttons.connected) write_report(buttons.h, 0);
    if (touch.connected)   write_report(touch.h, 0);
    if (dev != INVALID_HANDLE_VALUE) CloseHandle(dev);
    CloseHandle(buttons.h);
    CloseHandle(touch.h);
    return 0;
}

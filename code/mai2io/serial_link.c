#include "serial_link.h"

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <process.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "setupapi.lib")

static FILE *g_log_file = NULL;

static void log_msg(const char *fmt, ...) {
    if (!g_log_file) {
        g_log_file = fopen("mai2io.log", "a");
    }
    if (!g_log_file) return;
    
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log_file, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_log_file, fmt, args);
    va_end(args);
    fflush(g_log_file);
}

// =============================================================================
//  Pico USB identifiers
// =============================================================================
#define PICO_VID "VID_2E8A"
#define PICO_PID "PID_000A"

// =============================================================================
//  Packet constants (must match controller firmware)
// =============================================================================
#define PKT_SIZE  16
#define PKT_SYNC0 0xAAu
#define PKT_SYNC1 0x55u

// =============================================================================
//  Internal state & Shared Memory
// =============================================================================
static HANDLE g_serial = INVALID_HANDLE_VALUE;
static HANDLE g_thread = NULL;
static volatile bool g_stop = false;
static bool g_active = false;   // true after successful init

struct serial_shared_state {
    uint8_t  touch_1p[7];
    uint16_t gamebtn_1p;
    uint8_t  opbtn;
};

static HANDLE g_hMapFile = NULL;
static HANDLE g_hMutex = NULL;
static struct serial_shared_state *g_shared = NULL;
static bool g_is_primary = false;

// We use a critical section to synchronize threads *within* this process
static CRITICAL_SECTION g_lock;
static bool g_lock_init = false;

// =============================================================================
//  COM port discovery
// =============================================================================
static bool find_pico_port(char *out, size_t out_size) {
    char env_port[16];
    if (GetEnvironmentVariableA("MAI2IO_COM", env_port, sizeof(env_port)) > 0) {
        int port = atoi(env_port);
        if (port > 0) {
            snprintf(out, out_size, "COM%d", port);
            return true;
        }
    }

    HDEVINFO di = SetupDiGetClassDevsA(
        &GUID_DEVCLASS_PORTS, "USB", NULL, DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return false;

    SP_DEVINFO_DATA dd;
    dd.cbSize = sizeof(dd);
    bool found = false;

    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        char hw[512] = {0};
        DWORD data_type = 0;
        if (!SetupDiGetDeviceRegistryPropertyA(
                di, &dd, SPDRP_HARDWAREID, &data_type, (PBYTE)hw, sizeof(hw), NULL))
            continue;
        if (!strstr(hw, PICO_VID) || !strstr(hw, PICO_PID))
            continue;

        HKEY hkey = SetupDiOpenDevRegKey(
                di, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (hkey == INVALID_HANDLE_VALUE) {
            continue;
        }

        char port_name[32] = {0};
        DWORD port_name_size = sizeof(port_name);
        LONG result = RegQueryValueExA(
                hkey, "PortName", NULL, &data_type, (LPBYTE)port_name, &port_name_size);
        RegCloseKey(hkey);

        if (result != ERROR_SUCCESS) {
            continue;
        }

        if (strncmp(port_name, "COM", 3) == 0) {
            snprintf(out, out_size, "%s", port_name);
            found = true;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(di);
    return found;
}

// =============================================================================
//  Raw serial read (non-blocking)
// =============================================================================
static int raw_read(uint8_t *buf, int len) {
    if (g_serial == INVALID_HANDLE_VALUE) return -1;
    DWORD n = 0;
    if (!ReadFile(g_serial, buf, (DWORD)len, &n, NULL)) return -1;
    return (int)n;
}

// =============================================================================
//  Apply a validated packet into shared memory
// =============================================================================
static void apply_packet(const uint8_t *pkt) {
    if (!g_shared) return;
    EnterCriticalSection(&g_lock);

    uint8_t touch[7];
    memcpy(touch, pkt + 3, 7);
    uint16_t gamebtn = (uint16_t)pkt[10];

    uint8_t op = 0;
    if (pkt[12]) op |= 0x01;  // test
    if (pkt[13]) op |= 0x02;  // service
    if (pkt[14]) op |= 0x04;  // coin

    static uint8_t last_touch[7] = {0};
    static uint16_t last_gamebtn = 0;
    static uint8_t last_opbtn = 0;
    
    bool changed = (memcmp(last_touch, touch, 7) != 0) || (last_gamebtn != gamebtn) || (last_opbtn != op);

    if (changed) {
        // log_msg("packet: seq=%d touch=%02X %02X %02X %02X %02X %02X %02X gamebtn=%02X opbtn=%02X\n",
        //         pkt[2], touch[0], touch[1], touch[2], touch[3], 
        //         touch[4], touch[5], touch[6], gamebtn, op);
        memcpy(last_touch, touch, 7);
        last_gamebtn = gamebtn;
        last_opbtn = op;
    }

    // Write to shared memory so secondary process can read it
    memcpy(g_shared->touch_1p, touch, 7);
    g_shared->gamebtn_1p = gamebtn;
    g_shared->opbtn = op;

    LeaveCriticalSection(&g_lock);
}

// =============================================================================
//  Reader thread (Only runs in primary process)
// =============================================================================
static unsigned int __stdcall reader_proc(void *ctx) {
    (void)ctx;
    uint8_t buf[PKT_SIZE * 8];
    int buf_len = 0;

    while (!g_stop) {
        if (g_serial == INVALID_HANDLE_VALUE) {
            char port[32];
            if (find_pico_port(port, sizeof(port))) {
                char path[64];
                snprintf(path, sizeof(path), "\\\\.\\%s", port);
                HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    DCB dcb = {0};
                    dcb.DCBlength = sizeof(DCB);
                    if (GetCommState(h, &dcb)) {
                        dcb.BaudRate = CBR_115200;
                        dcb.ByteSize = 8;
                        dcb.StopBits = ONESTOPBIT;
                        dcb.Parity   = NOPARITY;
                        dcb.fDtrControl = DTR_CONTROL_ENABLE;
                        dcb.fRtsControl = RTS_CONTROL_ENABLE;
                        if (SetCommState(h, &dcb)) {
                            COMMTIMEOUTS to = {0};
                            to.ReadIntervalTimeout        = 50;
                            to.ReadTotalTimeoutConstant   = 50;
                            to.ReadTotalTimeoutMultiplier = 10;
                            SetCommTimeouts(h, &to);
                            g_serial = h;
                            log_msg("serial_link: connected on %s\n", port);
                        } else {
                            CloseHandle(h);
                        }
                    } else {
                        CloseHandle(h);
                    }
                }
            }
            if (g_serial == INVALID_HANDLE_VALUE) {
                Sleep(1000);
                continue;
            }
        }

        uint8_t tmp[PKT_SIZE * 4];
        int got = raw_read(tmp, sizeof(tmp));
        if (got < 0) { 
            CloseHandle(g_serial);
            g_serial = INVALID_HANDLE_VALUE;
            log_msg("serial_link: disconnected, will retry\n");
            Sleep(1000);
            continue; 
        }
        if (got == 0) { Sleep(1); continue; }

        if (buf_len + got > (int)sizeof(buf)) buf_len = 0;
        memcpy(buf + buf_len, tmp, (size_t)got);
        buf_len += got;

        int i = 0;
        while (i + PKT_SIZE <= buf_len) {
            if (buf[i] != PKT_SYNC0 || buf[i+1] != PKT_SYNC1) { i++; continue; }

            uint8_t chk = 0;
            for (int j = 2; j < PKT_SIZE - 1; j++) chk ^= buf[i + j];
            if (chk != buf[i + PKT_SIZE - 1]) { i++; continue; }

            apply_packet(buf + i);
            i += PKT_SIZE;
        }

        if (i > 0) {
            buf_len -= i;
            if (buf_len > 0) memmove(buf, buf + i, (size_t)buf_len);
        }
    }
    return 0;
}

// =============================================================================
//  Public API
// =============================================================================

bool serial_link_init(void) {
    if (!g_lock_init) {
        InitializeCriticalSection(&g_lock);
        g_lock_init = true;
    }

    // Create or Open Shared Memory
    g_hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(struct serial_shared_state), "Local\\Mai2SerialLink_SharedMemory");
    if (!g_hMapFile) {
        log_msg("serial_link: failed to create shared memory\n");
        return false;
    }

    g_shared = (struct serial_shared_state *)MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct serial_shared_state));
    if (!g_shared) {
        log_msg("serial_link: failed to map shared memory\n");
        return false;
    }

    // Elect Primary Process
    g_hMutex = CreateMutexA(NULL, TRUE, "Local\\Mai2SerialLink_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // We are secondary! Don't open COM port.
        CloseHandle(g_hMutex);
        g_hMutex = NULL;
        g_is_primary = false;
        g_active = true;
        log_msg("serial_link: secondary process attached to shared memory\n");
        return true;
    }
    
    // We are primary! Open COM port and run reader thread.
    g_is_primary = true;
    memset(g_shared, 0, sizeof(struct serial_shared_state));

    g_serial = INVALID_HANDLE_VALUE;

    // Start reader thread
    g_stop = false;
    g_thread = (HANDLE)_beginthreadex(NULL, 0, reader_proc, NULL, 0, NULL);
    g_active = true;

    log_msg("serial_link: primary process started, looking for Pico...\n");
    return true;
}

void serial_link_shutdown(void) {
    if (g_is_primary) {
        g_stop = true;
        if (g_thread) {
            WaitForSingleObject(g_thread, 2000);
            CloseHandle(g_thread);
            g_thread = NULL;
        }
        if (g_serial != INVALID_HANDLE_VALUE) {
            CloseHandle(g_serial);
            g_serial = INVALID_HANDLE_VALUE;
        }
        if (g_hMutex) {
            ReleaseMutex(g_hMutex);
            CloseHandle(g_hMutex);
            g_hMutex = NULL;
        }
    }
    
    if (g_shared) {
        UnmapViewOfFile(g_shared);
        g_shared = NULL;
    }
    if (g_hMapFile) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
    }

    if (g_lock_init) {
        DeleteCriticalSection(&g_lock);
        g_lock_init = false;
    }
    g_active = false;
}

void serial_link_get_touch_state(int player, uint8_t state[7]) {
    if (!g_active || !g_shared) return;
    EnterCriticalSection(&g_lock);
    if (player == 1) {
        for (int i = 0; i < 7; i++) state[i] |= g_shared->touch_1p[i];
    }
    LeaveCriticalSection(&g_lock);
}

void serial_link_get_gamebtns(int player, uint16_t *btns) {
    if (!g_active || !g_shared) return;
    EnterCriticalSection(&g_lock);
    if (player == 1) {
        *btns |= g_shared->gamebtn_1p;
    }
    LeaveCriticalSection(&g_lock);
}

void serial_link_get_opbtns(uint8_t *opbtn) {
    if (!g_active || !g_shared) return;
    EnterCriticalSection(&g_lock);
    *opbtn |= g_shared->opbtn;
    LeaveCriticalSection(&g_lock);
}
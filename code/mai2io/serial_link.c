#include "serial_link.h"

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "setupapi.lib")

// Raspberry Pi Pico USB identifiers.
// VID 0x2E8A is Raspberry Pi Foundation. PID 0x000A is the Pico SDK's
// default USB-CDC example PID -- change this to match your firmware.
#define PICO_VID "VID_2E8A"
#define PICO_PID "PID_000A"

static HANDLE g_serial_handle = INVALID_HANDLE_VALUE;

// Scans the Ports device class for a device whose hardware ID contains
// PICO_VID and PICO_PID, and extracts its COM port name (e.g. "COM5").
static bool find_pico_port(char *out_port, size_t out_port_size) {
    HDEVINFO dev_info = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE) {
        return false;
    }

    SP_DEVINFO_DATA dev_data;
    dev_data.cbSize = sizeof(SP_DEVINFO_DATA);
    bool found = false;

    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(dev_info, i, &dev_data); i++) {
        char hardware_id[256] = {0};
        if (!SetupDiGetDeviceRegistryPropertyA(dev_info, &dev_data, SPDRP_HARDWAREID,
                                                NULL, (PBYTE)hardware_id, sizeof(hardware_id), NULL)) {
            continue;
        }

        // Hardware ID looks like: USB\VID_2E8A&PID_000A\<serial>
        if (!strstr(hardware_id, PICO_VID) || !strstr(hardware_id, PICO_PID)) {
            continue;
        }

        // Friendly name contains "(COMx)" -- that's where the port lives.
        char friendly_name[256] = {0};
        if (!SetupDiGetDeviceRegistryPropertyA(dev_info, &dev_data, SPDRP_FRIENDLYNAME,
                                                NULL, (PBYTE)friendly_name, sizeof(friendly_name), NULL)) {
            continue;
        }

        char *open_paren = strstr(friendly_name, "(COM");
        if (!open_paren) continue;
        char *close_paren = strchr(open_paren, ')');
        if (!close_paren) continue;

        size_t len = (size_t)(close_paren - (open_paren + 1));
        if (len >= out_port_size) len = out_port_size - 1;

        memcpy(out_port, open_paren + 1, len);
        out_port[len] = '\0';
        found = true;
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return found;
}

bool serial_link_init(void) {
    char port_name[32];
    if (!find_pico_port(port_name, sizeof(port_name))) {
        fprintf(stderr, "serial_link_init: no Pico device found\n");
        return false;
    }

    // Ports above COM9 require the \\.\ prefix to open correctly.
    char full_path[64];
    snprintf(full_path, sizeof(full_path), "\\\\.\\%s", port_name);

    g_serial_handle = CreateFileA(full_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_serial_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "serial_link_init: failed to open %s (error %lu)\n", port_name, GetLastError());
        return false;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(g_serial_handle, &dcb)) {
        CloseHandle(g_serial_handle);
        g_serial_handle = INVALID_HANDLE_VALUE;
        return false;
    }
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity   = NOPARITY;
    if (!SetCommState(g_serial_handle, &dcb)) {
        CloseHandle(g_serial_handle);
        g_serial_handle = INVALID_HANDLE_VALUE;
        return false;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    SetCommTimeouts(g_serial_handle, &timeouts);

    fprintf(stderr, "serial_link_init: connected on %s\n", port_name);
    return true;
}

int serial_link_read(uint8_t *buf, int len) {
    if (g_serial_handle == INVALID_HANDLE_VALUE) return -1;
    DWORD bytes_read = 0;
    if (!ReadFile(g_serial_handle, buf, (DWORD)len, &bytes_read, NULL)) {
        return -1;
    }
    return (int)bytes_read;
}

void serial_link_close(void) {
    if (g_serial_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_serial_handle);
        g_serial_handle = INVALID_HANDLE_VALUE;
    }
}
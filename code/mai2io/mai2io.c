#include "mai2io.h"
#include "config.h"
#include "gui.h"

#include <limits.h>
#include <process.h>
#include <string.h>

static uint8_t mai2_opbtn;
static uint16_t mai2_player1_btn;
static uint16_t mai2_player2_btn;
static struct mai2_io_config mai2_io_cfg;
static bool mai2_io_coin;

static mai2_io_touch_callback_t _callback;

static HANDLE mai2_io_touch_1p_thread;
static bool mai2_io_touch_1p_stop_flag;

static HANDLE mai2_io_touch_2p_thread;
static bool mai2_io_touch_2p_stop_flag;

uint16_t mai2_io_get_api_version(void) {
    return 0x0102;
}

HRESULT mai2_io_init(void) {
    // We are simulating getting the path. In a real segatools DLL,
    // we would likely use GetModuleFileName to find our path and read segatools.ini next to it.
    // For simplicity, we just assume "segatools.ini" in the current directory.
    mai2_io_config_load(&mai2_io_cfg, L".\\segatools.ini");
    mai2_gui_init();
    return S_OK;
}

HRESULT mai2_io_poll(void) {
    mai2_opbtn = 0;
    mai2_player1_btn = 0;
    mai2_player2_btn = 0;

    // --- Keyboard Operator Buttons ---
    if (GetAsyncKeyState(mai2_io_cfg.vk_test) & 0x8000) {
        mai2_opbtn |= MAI2_IO_OPBTN_TEST;
    }
    if (GetAsyncKeyState(mai2_io_cfg.vk_service) & 0x8000) {
        mai2_opbtn |= MAI2_IO_OPBTN_SERVICE;
    }
    if (GetAsyncKeyState(mai2_io_cfg.vk_coin) & 0x8000) {
        if (!mai2_io_coin) {
            mai2_io_coin = true;
            mai2_opbtn |= MAI2_IO_OPBTN_COIN;
        }
    } else {
        mai2_io_coin = false;
    }

    // --- GUI Operator Buttons ---
    mai2_gui_get_opbtns(&mai2_opbtn);

    // --- Keyboard Player 1 Game Buttons ---
    if (mai2_io_cfg.vk_btn_enable) {
        for (int i = 0; i < 9; i++) {
            if (GetAsyncKeyState(mai2_io_cfg.vk_1p_btn[i]) & 0x8000) {
                mai2_player1_btn |= (1 << i);
            }
        }
        // Player 2 keyboard buttons are ignored because user requested GUI only for P2,
        // but if they really want them configured we could poll them here.
        // For now, adhering strictly to "all of player 2 input will go on gui"
    }

    // --- GUI Game Buttons ---
    mai2_gui_get_gamebtns(1, &mai2_player1_btn);
    mai2_gui_get_gamebtns(2, &mai2_player2_btn);

    return S_OK;
}

void mai2_io_get_opbtns(uint8_t *opbtn) {
    if (opbtn != NULL) {
        *opbtn = mai2_opbtn;
    }
}

void mai2_io_get_gamebtns(uint16_t *player1, uint16_t *player2) {
    if (player1 != NULL) {
        *player1 = mai2_player1_btn;
    }
    if (player2 != NULL) {
        *player2 = mai2_player2_btn;
    }
}

HRESULT mai2_io_touch_init(mai2_io_touch_callback_t callback) {
    _callback = callback;
    return S_OK;
}

void mai2_io_touch_set_sens(uint8_t *bytes) {
    // Stub
    return;
}

static unsigned int __stdcall mai2_io_touch_1p_thread_proc(void *ctx) {
    mai2_io_touch_callback_t callback = (mai2_io_touch_callback_t)ctx;

    while (!mai2_io_touch_1p_stop_flag) {
        uint8_t state[7] = {0, 0, 0, 0, 0, 0, 0};

        // Keyboard Touch (Debug Input)
        if (mai2_io_cfg.debug_input_1p) {
            for (int i = 0; i < 34; i++) {
                if (GetAsyncKeyState(mai2_io_cfg.vk_1p_touch[i]) & 0x8000) {
                    int byteIndex = i / 5;
                    int bitIndex = i % 5;
                    state[byteIndex] |= (1 << bitIndex);
                }
            }
        }

        // GUI Touch
        uint8_t gui_state[7] = {0};
        mai2_gui_get_touch_state(1, gui_state);
        for(int i=0; i<7; i++) {
            state[i] |= gui_state[i];
        }

        callback(1, state);
        Sleep(1);
    }
    return 0;
}

static unsigned int __stdcall mai2_io_touch_2p_thread_proc(void *ctx) {
    mai2_io_touch_callback_t callback = (mai2_io_touch_callback_t)ctx;

    while (!mai2_io_touch_2p_stop_flag) {
        uint8_t state[7] = {0, 0, 0, 0, 0, 0, 0};

        // GUI Touch only for P2
        uint8_t gui_state[7] = {0};
        mai2_gui_get_touch_state(2, gui_state);
        for(int i=0; i<7; i++) {
            state[i] |= gui_state[i];
        }

        callback(2, state);
        Sleep(1);
    }
    return 0;
}


void mai2_io_touch_update(bool player1, bool player2) {
    if (player1 && mai2_io_touch_1p_thread == NULL) {
        mai2_io_touch_1p_stop_flag = false;
        mai2_io_touch_1p_thread = (HANDLE)_beginthreadex(
            NULL, 0, mai2_io_touch_1p_thread_proc, (void*)_callback, 0, NULL);
    } else if (!player1 && mai2_io_touch_1p_thread != NULL) {
        mai2_io_touch_1p_stop_flag = true;
        WaitForSingleObject(mai2_io_touch_1p_thread, INFINITE);
        CloseHandle(mai2_io_touch_1p_thread);
        mai2_io_touch_1p_thread = NULL;
    }

    if (player2 && mai2_io_touch_2p_thread == NULL) {
        mai2_io_touch_2p_stop_flag = false;
        mai2_io_touch_2p_thread = (HANDLE)_beginthreadex(
            NULL, 0, mai2_io_touch_2p_thread_proc, (void*)_callback, 0, NULL);
    } else if (!player2 && mai2_io_touch_2p_thread != NULL) {
        mai2_io_touch_2p_stop_flag = true;
        WaitForSingleObject(mai2_io_touch_2p_thread, INFINITE);
        CloseHandle(mai2_io_touch_2p_thread);
        mai2_io_touch_2p_thread = NULL;
    }
}

HRESULT mai2_io_led_init(void) { return S_OK; }

void mai2_io_led_set_fet_output(uint8_t board, const uint8_t *rgb) {
    return;
}

void mai2_io_led_dc_update(uint8_t board, const uint8_t *rgb) {
    return;
}

void mai2_io_led_gs_update(uint8_t board, const uint8_t *rgb) {
    return;
}

void mai2_io_led_billboard_set(uint8_t board, const uint8_t *rgb) {
    return;
}

void mai2_io_led_cam_set(uint8_t state) {
    return;
}

// We also need to hook DLL process detach to shutdown the GUI cleanly.
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            break;
        case DLL_PROCESS_DETACH:
            mai2_gui_shutdown();
            break;
    }
    return TRUE;
}

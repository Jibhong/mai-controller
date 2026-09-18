#pragma once

#include <windows.h>

#include <stdint.h>
#include <stdbool.h>

enum {
    MAI2_IO_OPBTN_TEST = 0x01,
    MAI2_IO_OPBTN_SERVICE = 0x02,
    MAI2_IO_OPBTN_COIN = 0x04,
};

enum {
    MAI2_IO_GAMEBTN_1 = 0x01,
    MAI2_IO_GAMEBTN_2 = 0x02,
    MAI2_IO_GAMEBTN_3 = 0x04,
    MAI2_IO_GAMEBTN_4 = 0x08,
    MAI2_IO_GAMEBTN_5 = 0x10,
    MAI2_IO_GAMEBTN_6 = 0x20,
    MAI2_IO_GAMEBTN_7 = 0x40,
    MAI2_IO_GAMEBTN_8 = 0x80,
    MAI2_IO_GAMEBTN_SELECT = 0x100,
};

/* Get the version of the Maimai IO API that this DLL supports. This
   function should return a positive 16-bit integer, where the high byte is
   the major version and the low byte is the minor version (as defined by the
   Semantic Versioning standard).

   The latest API version as of this writing is 0x0102. */

__declspec(dllexport) uint16_t mai2_io_get_api_version(void);

/* Initialize the IO DLL. This is the second function that will be called on
   your DLL, after mai2_io_get_api_version.

   All subsequent calls to this API may originate from arbitrary threads.

   Minimum API version: 0x0100 */

__declspec(dllexport) HRESULT mai2_io_init(void);

/* Send any queued outputs (of which there are currently none, though this may
   change in subsequent API versions) and retrieve any new inputs.

   Minimum API version: 0x0100 */

__declspec(dllexport) HRESULT mai2_io_poll(void);

/* Get the state of the cabinet's operator buttons as of the last poll. See
   MAI2_IO_OPBTN enum above: this contains bit mask definitions for button
   states returned in *opbtn. All buttons are active-high.

   Minimum API version: 0x0100 */

__declspec(dllexport) void mai2_io_get_opbtns(uint8_t *opbtn);

/* Get the state of the cabinet's gameplay buttons as of the last poll. See
   MAI2_IO_GAMEBTN enum above for bit mask definitions. Inputs are split into
   a left hand side set of inputs and a right hand side set of inputs: the bit
   mappings are the same in both cases.

   All buttons are active-high, even though some buttons' electrical signals
   on a real cabinet are active-low.

   Minimum API version: 0x0100 */

__declspec(dllexport) void mai2_io_get_gamebtns(uint16_t *player1, uint16_t *player2);

/* Callback function used by mai2_io_touch_1p/2p_thread_proc. 

   The 'player'(1 or 2) parameter indicates which player the touch data is for.

   The 'state' represents a complete response packet.
   The format of the state array is as follows:
   uint8_t state[7] = {
      bytes[0] - bit(0 , 0 , 0 , A5, A4, A3, A2, A1)
      bytes[1] - bit(0 , 0 , 0 , B2, B1, A8, A7, A6)
      bytes[2] - bit(0 , 0 , 0 , B7, B6, B5, B4, B3)
      bytes[3] - bit(0 , 0 , 0 , D2, D1, C2, C1, B8)
      bytes[4] - bit(0 , 0 , 0 , D7, D6, D5, D4, D3)
      bytes[5] - bit(0 , 0 , 0 , E4, E3, E2, E1, D8)
      bytes[6] - bit(0 , 0 , 0 , 0 , E8, E7, E6, E5)
   }
   The 7 bytes are the touch data, with each byte storing the touch state in the lower 5 bits.
   A value of 1 indicates that the corresponding touch area is pressed.
   The touch areas are ordered from A1 to E8, and the binary values are stored from low to high. */

typedef void (*mai2_io_touch_callback_t)(const uint8_t player, const uint8_t state[7]);

__declspec(dllexport) HRESULT mai2_io_touch_init(mai2_io_touch_callback_t callback);

__declspec(dllexport) void mai2_io_touch_set_sens(uint8_t *bytes);

__declspec(dllexport) void mai2_io_touch_update(bool player1, bool player2);

/* Initialize LED emulation.

   Minimum API version: 0x0101 */

__declspec(dllexport) HRESULT mai2_io_led_init(void);

__declspec(dllexport) void mai2_io_led_set_fet_output(uint8_t board, const uint8_t *rgb);

__declspec(dllexport) void mai2_io_led_dc_update(uint8_t board, const uint8_t *rgb);

__declspec(dllexport) void mai2_io_led_gs_update(uint8_t board, const uint8_t *rgb);

/* Update the Billboard LEDs.

   Minimum API version: 0x0102 */

__declspec(dllexport) void mai2_io_led_billboard_set(uint8_t board, const uint8_t *rgb);

enum {
    MAI2_IO_LED_CAM_CODE_READER_1P = 0x01,
    MAI2_IO_LED_CAM_CODE_READER_2P = 0x02,
    MAI2_IO_LED_CAM_RING = 0x04,
    MAI2_IO_LED_CAM_REC = 0x08,
};

/* Update the Code Reader and Player Camera lights.

   Minimum API version: 0x0102 */

__declspec(dllexport) void mai2_io_led_cam_set(uint8_t state);

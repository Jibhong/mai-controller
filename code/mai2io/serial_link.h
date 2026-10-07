#pragma once

#include <stdint.h>
#include <stdbool.h>

// Initialize serial link: find Pico COM port, open it, start reader thread.
bool serial_link_init(void);

// Shut down the reader thread and close the serial port.
void serial_link_shutdown(void);

// Get current touch state from the hardware controller for the given player.
// Populates the 7-byte state array (same format as mai2_gui_get_touch_state).
void serial_link_get_touch_state(int player, uint8_t state[7]);

// Get current game button state from the hardware controller.
// ORs into *btns (same convention as mai2_gui_get_gamebtns).
void serial_link_get_gamebtns(int player, uint16_t *btns);

// Get current operator button state from the hardware controller.
// ORs into *opbtn (same convention as mai2_gui_get_opbtns).
void serial_link_get_opbtns(uint8_t *opbtn);

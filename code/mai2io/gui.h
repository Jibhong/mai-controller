#pragma once

#include <stdint.h>
#include <stdbool.h>

// Initializes the GUI window. Spawns a background thread.
void mai2_gui_init(void);

// Shuts down the GUI window and background thread.
void mai2_gui_shutdown(void);

// Reads the current touch state from the GUI for the given player (1 or 2).
// Populates the 7-byte state array.
void mai2_gui_get_touch_state(int player, uint8_t state[7]);

// Reads the current game buttons state from the GUI for the given player (1 or 2).
// Updates the value pointed to by btns.
void mai2_gui_get_gamebtns(int player, uint16_t *btns);

// Reads the current operator buttons state from the GUI.
// Updates the value pointed to by opbtn.
void mai2_gui_get_opbtns(uint8_t *opbtn);

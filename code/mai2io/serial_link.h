#pragma once

#include <stdint.h>

struct serial_link_frame_t {
    uint8_t  test_btn;
    uint8_t  service_btn;
    uint8_t  coin_btn;
    uint8_t  btn[9];
    uint8_t  touch[34];
};

int serial_link_init();

int serial_link_read_input();

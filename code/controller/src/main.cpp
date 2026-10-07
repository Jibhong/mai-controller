
#include <Arduino.h>
#include <USB.h>
#include <Wire.h>

extern "C" {
#include "mpr121.h"
}

// =============================================================================
//  MPR121 I2C Addresses
//  chip 0 = 0x5A, 1 = 0x5B, 2 = 0x5C
// =============================================================================
static constexpr uint8_t MPR121_ADDR[3] = {0x5A, 0x5B, 0x5C};

// How many MPR121 chips are actually connected and initialized.
// Must match the loop limit in setup().
#define NUM_MPR121  3

// =============================================================================
//  Action encoding
//
//  Each MPR121 pin maps to one of:
//    UNUSED          - ignored
//    TOUCH(row, col) - mai2 touch zone, using ROW_x constants below
//    BTN(n)          - game button 0-7 (bit n in game_btn byte)
//
//  Encoding in int16_t:
//    >= 0  -> flat touch zone index (see ROW_x offsets)
//    -1    -> UNUSED
//    < -1  -> game button: value = -(btn_index + 2)
//
//  mai2 touch zone flat index layout (34 zones total, 5 per byte):
//    A1-A8 = 0..7
//    B1-B8 = 8..15
//    C1-C2 = 16..17   (C only has 2 zones)
//    D1-D8 = 18..25
//    E1-E8 = 26..33
// =============================================================================
#define UNUSED       ((int16_t)(-1))
#define TOUCH(row_base, col) ((int16_t)((row_base) + (col) - 1))
#define BTN(n)       ((int16_t)(-((n) + 2)))

// Row base offsets into the 34-zone flat array
#define ROW_A  0
#define ROW_B  8
#define ROW_C  16   // C1=16, C2=17 only
#define ROW_D  18
#define ROW_E  26

// =============================================================================
//  *** EDIT THIS TABLE TO CONFIGURE YOUR WIRING ***
//
//  mpr_map[chip][pin]   chip = 0..2,  pin = 0..11
//
//  Example entries:
//    TOUCH(ROW_A, 1)  ->  touch zone A1
//    TOUCH(ROW_E, 8)  ->  touch zone E8
//    BTN(0)           ->  game button 1  (0x01 in mai2 game-btn word)
//    BTN(7)           ->  game button 8  (0x80)
//    UNUSED           ->  not connected
// =============================================================================
static const int16_t mpr_map[3][12] = {
    // --- MPR121 chip 0 (addr 0x5A) ---
    //  pin 0            pin 1            pin 2            pin 3
    //  pin 4            pin 5            pin 6            pin 7
    //  pin 8            pin 9            pin10            pin11
    {
        TOUCH(ROW_A, 2), TOUCH(ROW_C, 1), TOUCH(ROW_B, 2), TOUCH(ROW_D, 2),
        TOUCH(ROW_E, 2), TOUCH(ROW_A, 1), TOUCH(ROW_B, 1), TOUCH(ROW_D, 1),
        TOUCH(ROW_E, 1), TOUCH(ROW_A, 8), TOUCH(ROW_B, 8), UNUSED,
    },

    // --- MPR121 chip 1 (addr 0x5B) ---
    {
        TOUCH(ROW_A, 5), TOUCH(ROW_B, 5), TOUCH(ROW_D, 5), TOUCH(ROW_E, 5),
        TOUCH(ROW_A, 4), TOUCH(ROW_B, 4), TOUCH(ROW_D, 4), TOUCH(ROW_E, 4),
        TOUCH(ROW_A, 3), TOUCH(ROW_B, 3), TOUCH(ROW_D, 3), TOUCH(ROW_E, 3),
    },

    // --- MPR121 chip 2 (addr 0x5C) ---
    {
        TOUCH(ROW_D, 8), TOUCH(ROW_E, 8), TOUCH(ROW_A, 7), TOUCH(ROW_B, 7),
        TOUCH(ROW_D, 7), TOUCH(ROW_E, 7), TOUCH(ROW_C, 2), TOUCH(ROW_A, 6),
        TOUCH(ROW_B, 6), TOUCH(ROW_D, 6), TOUCH(ROW_E, 6), UNUSED,
    },
};

// =============================================================================
//  Packet layout (16 bytes total):
//
//   [0]      SYNC0  = 0xAA
//   [1]      SYNC1  = 0x55
//   [2]      seq    (rolling counter)
//   [3..9]   touch_state[7]  - mai2 A1-E8 bitfield (5 zones per byte, low 5 bits)
//   [10]     game_btn        - bits 0-7 = game buttons 1-8 (active-high)
//   [11]     air_packed      - bits 0-5 = air beams 0-5
//   [12]     test_btn
//   [13]     service_btn
//   [14]     coin_btn
//   [15]     checksum (XOR of bytes [2]..[14])
// =============================================================================
static constexpr uint8_t SYNC0 = 0xAA;
static constexpr uint8_t SYNC1 = 0x55;
static constexpr size_t  PACKET_SIZE = 16;

static uint8_t seq_counter = 0;

// Sensor state
static uint8_t touch_state[7] = {0}; // mai2 A1-E8 bitfield
static uint8_t game_btn       = 0;   // bits 0-7 -> BTN1-BTN8
static uint8_t air_beams[6]   = {0};
static uint8_t test_btn       = 0;
static uint8_t service_btn    = 0;
static uint8_t coin_btn       = 0;

//

void debug_mpr121(int mode) {
    static const uint8_t addrs[3] = {0x5A, 0x5B, 0x5C};

    for (int chip = 0; chip < 3; chip++) {
        uint8_t addr = addrs[chip];

        // Raw data: output pin_value_1 format (e.g. 756_1)
        if (mode & (1 << 0)) {
            uint16_t raw[12] = {0};
            uint16_t touched = mpr121_touched(addr);
            mpr121_raw(addr, raw, 12);

            for (int pin = 0; pin < 12; pin++) {
                uint8_t status = (touched >> pin) & 1;
                Serial.printf("%d_%d ", raw[pin], status);
            }
        } 
        // Touch state only if raw is not requested
        else if (mode & (1 << 1)) {
            uint16_t touched = mpr121_touched(addr);
            for (int pin = 0; pin < 12; pin++) {
                uint8_t status = (touched >> pin) & 1;
                Serial.printf("%d_%d ",pin, status);
            }
            Serial.printf("| ");
        }
    }
    Serial.print("\n");
}


// =============================================================================
//  Helpers: set one touch bit or one game-button bit
// =============================================================================

// zone_index: 0=A1, 1=A2 ... 7=A8, 8=B1 ... 33=E2  (34 zones total)
// Packed 5 zones per byte: byteIdx = zone/5, bitIdx = zone%5
static inline void set_touch_bit(uint8_t zone) {
    if (zone >= 35) return;
    touch_state[zone / 5] |= (uint8_t)(1u << (zone % 5));
}

// btn_index 0-7 -> bit 0-7 of game_btn
static inline void set_game_btn_bit(uint8_t btn_index) {
    if (btn_index >= 8) return;
    game_btn |= (uint8_t)(1u << btn_index);
}

// =============================================================================
//  scan_slider() - iterate all 3 MPR121s, apply the lookup table
// =============================================================================
static void scan_slider() {
    memset(touch_state, 0, sizeof(touch_state));
    game_btn = 0;

    for (uint8_t chip = 0; chip < NUM_MPR121; chip++) {
        uint16_t touched = mpr121_touched(MPR121_ADDR[chip]);

        for (uint8_t pin = 0; pin < 12; pin++) {
            if (!((touched >> pin) & 1u)) continue;

            int16_t action = mpr_map[chip][pin];
            if (action == UNUSED) continue;

            if (action >= 0) {
                set_touch_bit((uint8_t)action);
            } else {
                // Decode game button index: action = -(idx+2) => idx = -(action+2)
                uint8_t btn_idx = (uint8_t)(-(action + 2));
                set_game_btn_bit(btn_idx);
            }
        }
    }
}

// =============================================================================
//  Air sensor scan (non-blocking, one sensor per loop iteration)
// =============================================================================
static void scan_air() {
    static uint32_t last_air_time  = 0;
    static bool     air_is_on      = false;
    static uint8_t  current_sensor = 0;

    if (!air_is_on) {
        digitalWrite(current_sensor, HIGH);
        last_air_time = micros();
        air_is_on = true;
        return;
    }
    if (micros() - last_air_time < 1000) return;

    // LOW = receiver sees IR (not blocked); HIGH = blocked
    air_beams[current_sensor] = (digitalRead(6 + current_sensor) == LOW) ? 1 : 0;

    digitalWrite(current_sensor, LOW);
    air_is_on = false;
    if (++current_sensor > 5) current_sensor = 0;
}

// =============================================================================
//  Op-button scan (physical pins, active-low with pull-up)
// =============================================================================
static void scan_buttons() {
    test_btn    = (digitalRead(13) == LOW) ? 1 : 0;
    service_btn = (digitalRead(14) == LOW) ? 1 : 0;
    coin_btn    = (digitalRead(15) == LOW) ? 1 : 0;
}

// =============================================================================
//  Hold pin 16 LOW to re-init all MPR121 chips (recovery without full reboot)
// =============================================================================
static void scanReboot() {
    static bool isInit = false;
    if (!isInit) { pinMode(16, INPUT_PULLUP); isInit = true; }
    if (digitalRead(16) == LOW) {
        delay(20);
        if (digitalRead(16) == LOW) {
            for (uint8_t chip = 0; chip < 3; chip++)
                mpr121_init(MPR121_ADDR[chip]);
        }
    }
}

// =============================================================================
//  Build and send one packet
// =============================================================================
static void send_packet() {

    uint8_t pkt[PACKET_SIZE];
    size_t i = 0;

    pkt[i++] = SYNC0;
    pkt[i++] = SYNC1;
    pkt[i++] = seq_counter++;

    // Touch state (7 bytes: A1-E8 in mai2io format)
    memcpy(&pkt[i], touch_state, 7); i += 7;

    // Game buttons (bits 0-7 = BTN1-BTN8)
    pkt[i++] = game_btn;

    // Air beams packed into 1 byte (bits 0-5)
    uint8_t air_packed = 0;
    for (uint8_t b = 0; b < 6; b++) air_packed |= (air_beams[b] & 1u) << b;
    pkt[i++] = air_packed;

    // Op buttons
    pkt[i++] = test_btn;
    pkt[i++] = service_btn;
    pkt[i++] = coin_btn;

    // Checksum: XOR of bytes [2..14]
    uint8_t checksum = 0;
    for (size_t j = 2; j < i; j++) checksum ^= pkt[j];
    pkt[i++] = checksum;

    Serial.write(pkt, PACKET_SIZE);
}

// =============================================================================
//  setup()
// =============================================================================
void setup() {
    // Custom VID/PID so the PC serial link code can reliably find us
    USB.disconnect();
    USB.setVIDPID(0x2E8A, 0x000A);
    USB.setManufacturer("Jibhong");
    USB.setProduct("MaiController");
    USB.connect();

    Serial.begin(115200);

    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(16, INPUT_PULLUP);

    // Air TX (IR emitter) pins
    for (int p = 0; p <= 5; p++) {
        pinMode(p, OUTPUT);
        digitalWrite(p, LOW);
    }
    // Air RX (IR receiver) pins
    for (int p = 6; p <= 11; p++) {
        pinMode(p, INPUT_PULLUP);
    }

    // Op-button pins
    pinMode(13, INPUT_PULLUP);
    pinMode(14, INPUT_PULLUP);
    pinMode(15, INPUT_PULLUP);

    // I2C bus
    Wire1.setSDA(26);
    Wire1.setSCL(27);
    Wire1.begin();

    // Initialize all 3 MPR121 chips
    for (uint8_t chip = 0; chip < 3; chip++) {
        if (!mpr121_init(MPR121_ADDR[chip])) {
            Serial.print("MPR121 chip ");
            Serial.print(chip);
            Serial.print(" (0x5");
            Serial.print(0xA + chip, HEX);
            Serial.println(") not found - halting. Hold pin16 to reboot.");
            while (true) {
                digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
                delay(20);
                if (digitalRead(16) == LOW) {
                    delay(20);
                    if (digitalRead(16) == LOW) rp2040.reboot();
                }
            }
        }
    }
}

// =============================================================================
//  loop()
// =============================================================================
void loop() {
    scan_slider();
    scan_air();
    scan_buttons();

    send_packet();    
    // debug_mpr121((1<<0) | (1<<1));
    // debug_mpr121((1<<1));

    // Board LED mirrors any active input
    bool any_active = (game_btn != 0) || test_btn || service_btn || coin_btn;
    for (uint8_t b = 0; b < 7 && !any_active; b++)
        if (touch_state[b]) any_active = true;

    digitalWrite(LED_BUILTIN, any_active ? HIGH : LOW);

    scanReboot();

    delay(1);
}
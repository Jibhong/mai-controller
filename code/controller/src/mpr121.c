/*
 * MP121 Captive Touch Sensor
 * WHowe <github.com/whowechina>
 *
 */

#include <stdint.h>
#include <string.h>

#include "hardware/i2c.h"

#define I2C_PORT i2c1
#include "mpr121.h"

#define IO_TIMEOUT_US 1000

#define TOUCH_THRESHOLD_BASE 35
#define RELEASE_THRESHOLD_BASE 30

#define VDD 3.3f
#define AC_HEADROOM 0.5f            // datasheet says 0.7; 0.1 often makes autoconfig fail on big pads

#define MPR121_TOUCH_STATUS_REG 0x00
#define MPR121_OUT_OF_RANGE_STATUS_0_REG 0x02
#define MPR121_OUT_OF_RANGE_STATUS_1_REG 0x03
#define MPR121_ELECTRODE_FILTERED_DATA_REG 0x04
#define MPR121_BASELINE_VALUE_REG 0x1E

#define MPR121_MAX_HALF_DELTA_RISING_REG 0x2B
#define MPR121_NOISE_HALF_DELTA_RISING_REG 0x2C
#define MPR121_NOISE_COUNT_LIMIT_RISING_REG 0x2D
#define MPR121_FILTER_DELAY_COUNT_RISING_REG 0x2E
#define MPR121_MAX_HALF_DELTA_FALLING_REG 0x2F
#define MPR121_NOISE_HALF_DELTA_FALLING_REG 0x30
#define MPR121_NOISE_COUNT_LIMIT_FALLING_REG 0x31
#define MPR121_FILTER_DELAY_COUNT_FALLING_REG 0x32
#define MPR121_NOISE_HALF_DELTA_TOUCHED_REG 0x33
#define MPR121_NOISE_COUNT_LIMIT_TOUCHED_REG 0x34
#define MPR121_FILTER_DELAY_COUNT_TOUCHED_REG 0x35

#define MPR121_TOUCH_THRESHOLD_REG 0x41
#define MPR121_RELEASE_THRESHOLD_REG 0x42

#define MPR121_DEBOUNCE_REG 0x5B
#define MPR121_AFE_CONFIG_REG 0x5C
#define MPR121_FILTER_CONFIG_REG 0x5D
#define MPR121_ELECTRODE_CONFIG_REG 0x5E
#define MPR121_ELECTRODE_CURRENT_REG 0x5F
#define MPR121_ELECTRODE_CHARGE_TIME_REG 0x6C
#define MPR121_GPIO_CTRL_0_REG 0x73
#define MPR121_GPIO_CTRL_1_REG 0x74
#define MPR121_GPIO_DATA_REG 0x75
#define MPR121_GPIO_DIRECTION_REG 0x76
#define MPR121_GPIO_ENABLE_REG 0x77
#define MPR121_GPIO_DATA_SET_REG 0x78
#define MPR121_GPIO_DATA_CLEAR_REG 0x79
#define MPR121_GPIO_DATA_TOGGLE_REG 0x7A
#define MPR121_AUTOCONFIG_CONTROL_0_REG 0x7B
#define MPR121_AUTOCONFIG_CONTROL_1_REG 0x7C
#define MPR121_AUTOCONFIG_USL_REG 0x7D
#define MPR121_AUTOCONFIG_LSL_REG 0x7E
#define MPR121_AUTOCONFIG_TARGET_REG 0x7F
#define MPR121_SOFT_RESET_REG 0x80

static void write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
  uint8_t buf[] = {reg, val};
  i2c_write_blocking_until(I2C_PORT, addr, buf, 2, false,
                           time_us_64() + IO_TIMEOUT_US);
}

static uint8_t read_reg(uint8_t addr, uint8_t reg) {
  uint8_t value = 0;
  i2c_write_blocking_until(I2C_PORT, addr, &reg, 1, true,
                           time_us_64() + IO_TIMEOUT_US);
  i2c_read_blocking_until(I2C_PORT, addr, &value, 1, false,
                          time_us_64() + IO_TIMEOUT_US);
  return value;
}

bool mpr121_init(uint8_t i2c_addr) {
  write_reg(i2c_addr, 0x80, 0x63); // soft reset
  sleep_ms(2);
  write_reg(i2c_addr, 0x5E, 0x00); // stop mode while configuring

  // baseline filter
  // rising
  write_reg(i2c_addr, 0x2B, 1);
  write_reg(i2c_addr, 0x2C, 1);
  write_reg(i2c_addr, 0x2D, 0);
  write_reg(i2c_addr, 0x2E, 0);
  // falling (was 1,1,6,12 -> much faster)
  write_reg(i2c_addr, 0x2F, 1);
  write_reg(i2c_addr, 0x30, 1);
  write_reg(i2c_addr, 0x31, 2);
  write_reg(i2c_addr, 0x32, 2);
  // touched (was 1,8,30)
  write_reg(i2c_addr, 0x33, 1);
  write_reg(i2c_addr, 0x34, 4);
  write_reg(i2c_addr, 0x35, 8);

  // Sensitive thresholds since filtering is off and pad is resistive
  for (int i = 0; i < 12; i++) {
    write_reg(i2c_addr, 0x41 + i * 2, TOUCH_THRESHOLD_BASE);   // e.g., 6 - 8
    write_reg(i2c_addr, 0x42 + i * 2, RELEASE_THRESHOLD_BASE); // e.g., 3 - 4
  }

  // 1. DEBOUNCE: 0 samples (immediate trigger, 0ms debounce delay)
  write_reg(i2c_addr, 0x5B, 0x00);

  // 2. AFE: FFI = 6 samples (0b00) [MIN FILTER], CDC start = 32uA
  write_reg(i2c_addr, 0x5C, 0x20);

  // 3. Filter/Timing: CDT = 32us (0b111) [MAX CHARGE], SFI = 4 (0b00) [MIN FILTER], ESI = 1ms (0b000)
  write_reg(i2c_addr, 0x5D, 0xE0);

  // 4. Autoconfig 1: FFI=6 (0b00 to match 0x5C), RETRY=4x, BVA=10, ARE=1, ACE=1
  write_reg(i2c_addr, 0x7B, 0x2B);
  
  // 5. Autoconfig 2: Enable search
  write_reg(i2c_addr, 0x7C, 0x00);

  // Target voltage levels
  const uint8_t usl = (VDD - AC_HEADROOM) / VDD * 256;
  write_reg(i2c_addr, 0x7D, usl);
  write_reg(i2c_addr, 0x7E, usl * 0.55);
  write_reg(i2c_addr, 0x7F, usl * 0.80); 

  // Enable all 12 electrodes + Auto-config
  for (int tries = 0; tries < 5; tries++) {
    write_reg(i2c_addr, 0x5E, 0x00);
    write_reg(i2c_addr, 0x5E, 0x8C);
    sleep_ms(50);
    
    if ((read_reg(i2c_addr, 0x02) & 0xC0) == 0) break; 
  }

  return read_reg(i2c_addr, 0x5E) == 0x8C;
}

#define ABS(x) ((x) < 0 ? -(x) : (x))

static void mpr121_read_many(uint8_t addr, uint8_t reg, uint8_t *buf, int num) {
  i2c_write_blocking_until(I2C_PORT, addr, &reg, 1, true,
                           time_us_64() + IO_TIMEOUT_US);
  i2c_read_blocking_until(I2C_PORT, addr, buf, num, false,
                          time_us_64() + IO_TIMEOUT_US * num / 2);
}

static void mpr121_read_many16(uint8_t addr, uint8_t reg, uint16_t *buf,
                               int num) {
  uint8_t vals[num * 2];
  memset(vals, 0, sizeof(vals));

  mpr121_read_many(addr, reg, vals, num * 2);
  for (int i = 0; i < num; i++) {
    buf[i] = (vals[i * 2 + 1] << 8) | vals[i * 2];
  }
}

uint16_t mpr121_touched(uint8_t addr) {
  uint16_t touched = 0;
  mpr121_read_many16(addr, MPR121_TOUCH_STATUS_REG, &touched, 1);
  return touched;
}

void mpr121_raw(uint8_t addr, uint16_t *raw, int num) {
  mpr121_read_many16(addr, MPR121_ELECTRODE_FILTERED_DATA_REG, raw, num);
}

static uint8_t mpr121_stop(uint8_t addr) {
  uint8_t ecr = read_reg(addr, MPR121_ELECTRODE_CONFIG_REG);
  write_reg(addr, MPR121_ELECTRODE_CONFIG_REG, ecr & 0xC0);
  return ecr;
}

static void mpr121_resume(uint8_t addr, uint8_t ecr) {
  write_reg(addr, MPR121_ELECTRODE_CONFIG_REG, ecr);
}

void mpr121_filter(uint8_t addr, uint8_t ffi, uint8_t sfi, uint8_t esi) {
  uint8_t ecr = mpr121_stop(addr);

  uint8_t afe = read_reg(addr, MPR121_AFE_CONFIG_REG);
  write_reg(addr, MPR121_AFE_CONFIG_REG, (afe & 0x3f) | ffi << 6);
  uint8_t acc = read_reg(addr, MPR121_AUTOCONFIG_CONTROL_0_REG);
  write_reg(addr, MPR121_AUTOCONFIG_CONTROL_0_REG, (acc & 0x3f) | ffi << 6);
  uint8_t fcr = read_reg(addr, MPR121_FILTER_CONFIG_REG);
  write_reg(addr, MPR121_FILTER_CONFIG_REG,
            (fcr & 0xe0) | ((sfi & 3) << 3) | esi);

  mpr121_resume(addr, ecr);
}

void mpr121_sense(uint8_t addr, int8_t sense, int8_t *sense_keys, int num) {
  uint8_t ecr = mpr121_stop(addr);
  for (int i = 0; (i < num) && (i < 12); i++) {
    int delta = sense + sense_keys[i];
    int t = TOUCH_THRESHOLD_BASE - delta;
    if (t < 4) t = 4;
    if (t > 255) t = 255;
    int r = t * 3 / 4;              // keep release below touch
    if (r > t - 2) r = t - 2;
    if (r < 1) r = 1;
    write_reg(addr, MPR121_TOUCH_THRESHOLD_REG + i * 2, t);
    write_reg(addr, MPR121_RELEASE_THRESHOLD_REG + i * 2, r);
  }
  mpr121_resume(addr, ecr);
}

void mpr121_debounce(uint8_t addr, uint8_t touch, uint8_t release) {
  uint8_t ecr = mpr121_stop(addr);
  write_reg(addr, 0x5B, (release & 0x07) << 4 | (touch & 0x07));
  mpr121_resume(addr, ecr);
}
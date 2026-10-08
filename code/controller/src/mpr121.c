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

/* ========================= I2C ========================= */
#define IO_TIMEOUT_US          1000   // per-transfer I2C timeout (us)

/* ================== TOUCH / RELEASE THRESHOLDS ==================
 * Compared against (baseline - filtered data), in raw counts.
 * Lower = more sensitive and faster trigger, but more noise-prone.
 * Release must stay below touch (hysteresis) or it will chatter.
 * Per-electrode trim is applied in mpr121_sense().
 */
#define TOUCH_THRESHOLD_BASE   25   // counts of change to register touch (1-255)
#define RELEASE_THRESHOLD_BASE 20     // counts to register release (1-255), < touch

/* ============== DEBOUNCE (reg 0x5B) ==============
 * Number of consecutive scan cycles a state must hold before it is reported.
 * 0 = instant (lowest latency). Max 7. Each step adds one full scan cycle.
 */
#define DEBOUNCE_TOUCH         0      // cycles before reporting touch (0-7)
#define DEBOUNCE_RELEASE       0      // cycles before reporting release (0-7)

/* ============== SAMPLING / FILTER (regs 0x5C, 0x5D) ==============
 * FFI: first filter iterations (samples averaged per scan, in AFE reg)
 *      0 = 6, 1 = 10, 2 = 18, 3 = 34.   Lower = faster, noisier.
 * SFI: second filter iterations (number of scan results averaged)
 *      0 = 4, 1 = 6, 2 = 10, 3 = 18.    Lower = faster, noisier.
 * ESI: electrode sample interval (period between scans)
 *      0 = 1ms, 1 = 2ms, 2 = 4ms ... 7 = 128ms.  0 = fastest response.
 */
#define FILTER_FFI             1      // 0..3
#define FILTER_SFI             1      // 0..3
#define FILTER_ESI             0      // 0..7

/* ============== CHARGE (MANUAL MODE, autoconfig off) ==============
 * Signal level is proportional to CDC x CDT.
 * Raw reading all 1023 -> too much charge, lower CDC or CDT.
 * Baseline under ~200 -> too little charge, raise them.
 * Target: untouched baseline ~400-700, touch delta >= 30 counts.
 * Prefer short CDT + higher CDC for faster scanning.
 *
 * CDC = charge current in uA, 0-63 (per electrode).
 * CDT codes: 1=0.5us 2=1us 3=2us 4=4us 5=8us 6=16us 7=32us
 */
#define MANUAL_CDC             15     // charge current, 0-63 uA
#define MANUAL_CDT             3      // charge time code, 1-7
#define GLOBAL_CDC             0      // global CDC in AFE reg (unused when per-electrode set)

/* ============== AUTOCONFIG (alternative to manual charge) ==============
 * 1 = chip picks CDC/CDT per electrode at startup (ignores MANUAL_CDC/CDT).
 * 0 = use manual values above.
 */
#define USE_AUTOCONFIG         0

#define VDD                    3.3f
#define AC_HEADROOM            0.5f   // datasheet says 0.7; lower avoids autoconfig failure on big pads
#define AC_USL_F               1.00f  // upper limit scale (of VDD - headroom)
#define AC_LSL_F               0.55f  // lower limit (fraction of USL)
#define AC_TARGET_F            0.80f  // target baseline (fraction of USL), higher = bigger delta
#define AC_RETRY               2      // retries on fail: 0=off 1=2x 2=4x 3=8x
#define AC_BVA                 2      // baseline value adjust after autoconfig (0-3)
#define AC_ARE                 1      // auto-reconfig on out-of-range
#define AC_ACE                 1      // auto-config enable

/* ============== BASELINE FILTER (auto-recovery / drift tracking) ==============
 * The baseline is a slow-moving reference the chip subtracts from the signal.
 * Each direction has up to 4 parameters:
 *   MHD  max half delta    (1-63)  max step the baseline moves per update; bigger = faster tracking
 *   NHD  noise half delta  (1-63)  step size used for small changes; bigger = faster tracking
 *   NCL  noise count limit (0-255) consecutive samples needed before the baseline moves; lower = faster
 *   FDL  filter delay count(0-255) extra delay between baseline updates; lower = faster
 * Fast values = quick recovery from drift or stuck state, but a held touch
 * gets absorbed into the baseline and "releases" by itself.
 * Slow values = held touches stay valid, but recovery from drift is slow.
 */
// RISING: signal above baseline (e.g. after release, baseline re-converges up)
#define BL_RISE_MHD            1
#define BL_RISE_NHD            1
#define BL_RISE_NCL            4
#define BL_RISE_FDL            2

// FALLING: signal below baseline (touch direction, keep slower if you want holds)
#define BL_FALL_MHD            1
#define BL_FALL_NHD            1
#define BL_FALL_NCL            64
#define BL_FALL_FDL            64

// TOUCHED: baseline behavior while an electrode is touched (no MHD register)
#define BL_TOUCH_NHD           1
#define BL_TOUCH_NCL           4
#define BL_TOUCH_FDL           8

/* ============== ELECTRODE CONFIG (reg 0x5E) ==============
 * ECR_CL: baseline tracking / initial load
 *   0 = tracking on, start from existing baseline
 *   1 = tracking off
 *   2 = tracking on, init baseline from first reading (5 MSBs)
 *   3 = tracking on, init baseline from first reading (full 10 bits)
 * ELECTRODE_COUNT: electrodes scanned (1-12). Fewer = faster scan cycle.
 */
#define ECR_CL                 2
#define ELECTRODE_COUNT        12

#define INIT_RETRIES           5      // retries until out-of-range flags clear
#define INIT_SETTLE_MS         50     // wait after enabling electrodes

/* ========================= REGISTERS ========================= */
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
  const uint8_t ecr_run = (ECR_CL << 6) | (ELECTRODE_COUNT & 0x0F);

  write_reg(i2c_addr, MPR121_SOFT_RESET_REG, 0x63);
  sleep_ms(2);
  write_reg(i2c_addr, MPR121_ELECTRODE_CONFIG_REG, 0x00); // stop mode while configuring

  // baseline filter: rising
  write_reg(i2c_addr, MPR121_MAX_HALF_DELTA_RISING_REG, BL_RISE_MHD);
  write_reg(i2c_addr, MPR121_NOISE_HALF_DELTA_RISING_REG, BL_RISE_NHD);
  write_reg(i2c_addr, MPR121_NOISE_COUNT_LIMIT_RISING_REG, BL_RISE_NCL);
  write_reg(i2c_addr, MPR121_FILTER_DELAY_COUNT_RISING_REG, BL_RISE_FDL);
  // falling
  write_reg(i2c_addr, MPR121_MAX_HALF_DELTA_FALLING_REG, BL_FALL_MHD);
  write_reg(i2c_addr, MPR121_NOISE_HALF_DELTA_FALLING_REG, BL_FALL_NHD);
  write_reg(i2c_addr, MPR121_NOISE_COUNT_LIMIT_FALLING_REG, BL_FALL_NCL);
  write_reg(i2c_addr, MPR121_FILTER_DELAY_COUNT_FALLING_REG, BL_FALL_FDL);
  // touched
  write_reg(i2c_addr, MPR121_NOISE_HALF_DELTA_TOUCHED_REG, BL_TOUCH_NHD);
  write_reg(i2c_addr, MPR121_NOISE_COUNT_LIMIT_TOUCHED_REG, BL_TOUCH_NCL);
  write_reg(i2c_addr, MPR121_FILTER_DELAY_COUNT_TOUCHED_REG, BL_TOUCH_FDL);

  // thresholds
  for (int i = 0; i < 12; i++) {
    write_reg(i2c_addr, MPR121_TOUCH_THRESHOLD_REG + i * 2, TOUCH_THRESHOLD_BASE);
    write_reg(i2c_addr, MPR121_RELEASE_THRESHOLD_REG + i * 2, RELEASE_THRESHOLD_BASE);
  }

  // debounce
  write_reg(i2c_addr, MPR121_DEBOUNCE_REG,
            ((DEBOUNCE_RELEASE & 7) << 4) | (DEBOUNCE_TOUCH & 7));

  // AFE: FFI + global CDC
  write_reg(i2c_addr, MPR121_AFE_CONFIG_REG,
            ((FILTER_FFI & 3) << 6) | (GLOBAL_CDC & 0x3F));

#if USE_AUTOCONFIG
  // global CDT = max, SFI/ESI from config
  write_reg(i2c_addr, MPR121_FILTER_CONFIG_REG,
            (7 << 5) | ((FILTER_SFI & 3) << 3) | (FILTER_ESI & 7));

  write_reg(i2c_addr, MPR121_AUTOCONFIG_CONTROL_0_REG,
            ((FILTER_FFI & 3) << 6) | ((AC_RETRY & 3) << 4) |
            ((AC_BVA & 3) << 2) | ((AC_ARE & 1) << 1) | (AC_ACE & 1));
  write_reg(i2c_addr, MPR121_AUTOCONFIG_CONTROL_1_REG, 0x00);

  const uint8_t usl = (VDD - AC_HEADROOM) / VDD * 256 * AC_USL_F;
  write_reg(i2c_addr, MPR121_AUTOCONFIG_USL_REG, usl);
  write_reg(i2c_addr, MPR121_AUTOCONFIG_LSL_REG, usl * AC_LSL_F);
  write_reg(i2c_addr, MPR121_AUTOCONFIG_TARGET_REG, usl * AC_TARGET_F);
#else
  // global CDT = 0 (per-electrode CDT used), SFI/ESI from config
  write_reg(i2c_addr, MPR121_FILTER_CONFIG_REG,
            ((FILTER_SFI & 3) << 3) | (FILTER_ESI & 7));

  // autoconfig off
  write_reg(i2c_addr, MPR121_AUTOCONFIG_CONTROL_0_REG, 0x00);
  write_reg(i2c_addr, MPR121_AUTOCONFIG_CONTROL_1_REG, 0x00);

  // manual per-electrode CDC and CDT
  for (int i = 0; i < 12; i++)
    write_reg(i2c_addr, MPR121_ELECTRODE_CURRENT_REG + i, MANUAL_CDC & 0x3F);
  for (int i = 0; i < 6; i++)
    write_reg(i2c_addr, MPR121_ELECTRODE_CHARGE_TIME_REG + i,
              (MANUAL_CDT & 7) | ((MANUAL_CDT & 7) << 4));
#endif

  // enable electrodes
  for (int tries = 0; tries < INIT_RETRIES; tries++) {
    write_reg(i2c_addr, MPR121_ELECTRODE_CONFIG_REG, 0x00);
    write_reg(i2c_addr, MPR121_ELECTRODE_CONFIG_REG, ecr_run);
    sleep_ms(INIT_SETTLE_MS);

    if ((read_reg(i2c_addr, MPR121_OUT_OF_RANGE_STATUS_0_REG) & 0xC0) == 0) break;
  }

  return read_reg(i2c_addr, MPR121_ELECTRODE_CONFIG_REG) == ecr_run;
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
  return touched & 0x0FFF;
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
  write_reg(addr, MPR121_DEBOUNCE_REG, (release & 0x07) << 4 | (touch & 0x07));
  mpr121_resume(addr, ecr);
}
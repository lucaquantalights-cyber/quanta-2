// hal.h — drivers for the chips on the controller board.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "core.h"

namespace hal {

// ======================================================================= MCP4728 quad DAC
// Two chips give 8 outputs. The DAC keeps its output while the ESP32 resets (watchdog, update,
// crash), so the lights don't blink. Its EEPROM decides the power-up value: we keep that at 0 V,
// then the firmware ramps up to the scheduled level.
// On the PCB, DAC A (U9) is on Wire (GPIO21/22) and DAC B (U8) on Wire1 (GPIO16/17), both at 0x64.
struct Mcp4728 {
  uint8_t addr;
  TwoWire *bus;
  bool ok = false;
  explicit Mcp4728(uint8_t a, TwoWire *b = &Wire) : addr(a), bus(b) {}

  bool begin() {
    bus->beginTransmission(addr);
    ok = bus->endTransmission() == 0;
    if (!ok) return false;
    bus->beginTransmission(addr); bus->write(0x8F); bus->endTransmission();          // internal 2.048 V ref, all channels
    bus->beginTransmission(addr); bus->write(0xC0); bus->endTransmission();          // gain x1, all channels
    bus->beginTransmission(addr); bus->write(0xA0); bus->write(0x00); bus->endTransmission();  // powered up
    return true;
  }

  // Read the 4 output registers and 4 EEPROM values.
  bool read(uint16_t cur[4], uint16_t eep[4], bool eepIntRef[4]) {
    if (bus->requestFrom((int)addr, 24) != 24) return false;
    for (int c = 0; c < 4; c++) {
      uint8_t b[6]; for (int i = 0; i < 6; i++) b[i] = bus->read();
      cur[c] = ((b[1] & 0x0F) << 8) | b[2];
      eep[c] = ((b[4] & 0x0F) << 8) | b[5];
      eepIntRef[c] = b[4] & 0x80;
    }
    return true;
  }

  // Make sure the chip powers up at 0 V with the internal reference. Writes EEPROM only if needed
  // (normally once in the unit's life), because EEPROM has limited write cycles.
  bool ensurePowerUpZero() {
    uint16_t cur[4], eep[4]; bool ir[4];
    if (!read(cur, eep, ir)) return false;
    bool need = false;
    for (int c = 0; c < 4; c++) if (eep[c] != 0 || !ir[c]) need = true;
    if (!need) return true;
    // Happens once per chip (factory-fresh). Outputs briefly go to 0 V, then are restored.
    for (int c = 0; c < 4; c++) {
      bus->beginTransmission(addr);
      bus->write(0x58 | (c << 1));                      // single write DAC+EEPROM, channel c
      bus->write(0x80); bus->write(0x00);
      bus->endTransmission();
      delay(60);
    }
    write(cur);                                         // put the live outputs back where they were
    return true;
  }

  // Fast write: all 4 outputs, volatile registers only. Output updates immediately (LDAC tied low).
  bool write(const uint16_t code[4]) {
    bus->beginTransmission(addr);
    for (int c = 0; c < 4; c++) { bus->write((code[c] >> 8) & 0x0F); bus->write(code[c] & 0xFF); }
    return bus->endTransmission() == 0;
  }
};

// ======================================================================= output monitor ADC
// PCB: one ADS7828 (8 channels, 2.5 V internal reference) at 0x48. Monitor1..8 -> CH0..CH7.
// Breadboard: ADS1115 at 0x48 (channels 1-4) and optionally a second at 0x49 (channels 5-8).
// Both sit at 0x48, so begin() asks the chip which one it is: an ADS7828 answers any read with
// four leading zero bits, an ADS1115's config register reads 0x8583 (top bit set).
struct MonitorAdc {
  enum Kind : uint8_t { NONE, ADS7828, ADS1115 };
  Kind kind = NONE;
  bool ok = false;
  bool second = false;                                  // second ADS1115 present (breadboard)

  static bool present(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }

  bool begin() {
    ok = present(ADDR_ADC);
    if (!ok) { kind = NONE; return false; }
    Wire.beginTransmission(ADDR_ADC); Wire.write(0x01);  // ADS1115: point at config register
    Wire.endTransmission();
    uint8_t hi = 0;
    if (Wire.requestFrom((int)ADDR_ADC, 2) == 2) { hi = Wire.read(); Wire.read(); }
    kind = (hi & 0xF0) ? ADS1115 : ADS7828;
    second = kind == ADS1115 && present(ADDR_ADC2);
    return true;
  }

  // Returns millivolts at the op-amp output (after undoing the divider), or -1.
  int readAmpMv(uint8_t ch) {
    if (!ok || ch > 7) return -1;
    float v = kind == ADS7828 ? read7828(ch) : read1115(ch);
    if (v < 0) return -1;
    return (int)(v * MON_DIVIDER * 1000.0f + 0.5f);
  }

private:
  // ADS7828: single-ended channel select bits are interleaved.
  static uint8_t sel(uint8_t ch) { static const uint8_t m[8] = {0, 4, 1, 5, 2, 6, 3, 7}; return m[ch & 7]; }
  float read7828(uint8_t ch) {
    Wire.beginTransmission(ADDR_ADC);
    Wire.write(0x80 | (sel(ch) << 4) | 0x0C);           // single-ended, internal ref on, ADC on
    if (Wire.endTransmission() != 0) return -1;
    if (Wire.requestFrom((int)ADDR_ADC, 2) != 2) return -1;
    uint16_t raw = ((Wire.read() & 0x0F) << 8) | Wire.read();
    return raw * ADC_REF_V / 4096.0f;
  }

  // ADS1115: single-shot, +/-4.096 V range, 860 samples/s (about 1.2 ms per reading).
  bool rd16(uint8_t a, uint8_t reg, uint16_t &v) {
    Wire.beginTransmission(a); Wire.write(reg);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((int)a, 2) != 2) return false;
    v = (uint16_t)Wire.read() << 8; v |= Wire.read();
    return true;
  }
  float read1115(uint8_t ch) {
    uint8_t a = ch < 4 ? ADDR_ADC : ADDR_ADC2;
    if (ch >= 4 && !second) return -1;
    uint16_t cfgw = 0x8000 | ((uint16_t)(4 + (ch & 3)) << 12) | (1 << 9) | (1 << 8) | (7 << 5) | 0x0003;
    Wire.beginTransmission(a); Wire.write(0x01); Wire.write(cfgw >> 8); Wire.write(cfgw & 0xFF);
    if (Wire.endTransmission() != 0) return -1;
    uint32_t t0 = millis(); uint16_t r;
    do { delay(1); if (!rd16(a, 0x01, r) || millis() - t0 > 10) return -1; } while (!(r & 0x8000));
    if (!rd16(a, 0x00, r)) return -1;
    int16_t s = (int16_t)r; if (s < 0) s = 0;           // monitor diode keeps the input at or above ~0 V
    return s * 4.096f / 32768.0f;
  }
};

// ======================================================================= DS3231 RTC (kept in UTC)
struct Ds3231 {
  uint8_t addr = ADDR_RTC;
  bool ok = false;
  static uint8_t bcd2(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
  static uint8_t tobcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

  bool begin() { Wire.beginTransmission(addr); ok = Wire.endTransmission() == 0; return ok; }

  // Days from civil date (Howard Hinnant's algorithm), no timegm() needed.
  static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
  }

  // Returns epoch seconds, or 0 if the clock has lost time (oscillator stopped / battery dead).
  uint32_t read() {
    if (!ok) return 0;
    Wire.beginTransmission(addr); Wire.write(0x0F);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)addr, 1) != 1) return 0;
    if (Wire.read() & 0x80) return 0;                   // OSF: time is not trustworthy
    Wire.beginTransmission(addr); Wire.write(0x00);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)addr, 7) != 7) return 0;
    uint8_t r[7]; for (int i = 0; i < 7; i++) r[i] = Wire.read();
    int sec = bcd2(r[0] & 0x7F), min = bcd2(r[1]), hr = bcd2(r[2] & 0x3F);
    int day = bcd2(r[4]), mon = bcd2(r[5] & 0x1F), yr = 2000 + bcd2(r[6]) + ((r[5] & 0x80) ? 100 : 0);
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hr > 23 || min > 59 || sec > 59) return 0;
    int64_t e = (int64_t)daysFromCivil(yr, mon, day) * 86400 + hr * 3600 + min * 60 + sec;
    return e > 1700000000 && e < 4000000000LL ? (uint32_t)e : 0;
  }

  bool write(uint32_t epoch) {
    if (!ok) return false;
    time_t t = epoch; struct tm u; gmtime_r(&t, &u);
    Wire.beginTransmission(addr); Wire.write(0x00);
    Wire.write(tobcd(u.tm_sec)); Wire.write(tobcd(u.tm_min)); Wire.write(tobcd(u.tm_hour));
    Wire.write(tobcd(u.tm_wday + 1)); Wire.write(tobcd(u.tm_mday));
    Wire.write(tobcd(u.tm_mon + 1)); Wire.write(tobcd((u.tm_year + 1900) % 100));
    if (Wire.endTransmission() != 0) return false;
    Wire.beginTransmission(addr); Wire.write(0x0F); Wire.write(0x00);   // clear OSF
    return Wire.endTransmission() == 0;
  }

  // Chip temperature, °C x 4 (it sits inside the controller box). INT16_MIN if unavailable.
  int16_t tempQuarterC() {
    if (!ok) return INT16_MIN;
    Wire.beginTransmission(addr); Wire.write(0x11);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom((int)addr, 2) != 2) return INT16_MIN;
    int8_t msb = (int8_t)Wire.read(); uint8_t lsb = Wire.read();
    return (int16_t)(msb * 4 + (lsb >> 6));
  }
};

}  // namespace hal

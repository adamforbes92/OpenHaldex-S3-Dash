// =====================================================================
// pca9557.cpp - minimal PCA9557 I2C I/O expander driver
// =====================================================================
#include "hal/pca9557.h"

#include <Arduino.h>
#include <Wire.h>

namespace {
// PCA9557 register map (identical to the PCA9554 family).
constexpr uint8_t REG_INPUT = 0x00;
constexpr uint8_t REG_OUTPUT = 0x01;
constexpr uint8_t REG_POLARITY = 0x02;
constexpr uint8_t REG_CONFIG = 0x03;
constexpr uint8_t BASE_ADDR = 0x18;  // 0x18..0x1F for A2:A0 = 000..111
}  // namespace

bool Pca9557::begin(uint8_t addr) {
  // Probe the expected address first (0x18 = A2:A0 all low), then the rest
  // of the family, so a board with different strapping still comes up.
  uint8_t candidates[9];
  uint8_t n = 0;
  if (addr != 0) {
    candidates[n++] = addr;
  } else {
    candidates[n++] = BASE_ADDR;
    for (uint8_t i = 0; i < 7; i++) candidates[n++] = BASE_ADDR + 1 + i;
  }

  for (uint8_t i = 0; i < n; i++) {
    Wire.beginTransmission(candidates[i]);
    if (Wire.endTransmission() != 0) continue;

    // A device that answers is not necessarily a PCA9557 - verify that the
    // configuration register reads back what we write.
    addr_ = candidates[i];  // writeReg()/readReg() use this

    uint8_t cfg = 0;
    if (!readReg(REG_CONFIG, cfg)) { addr_ = 0; continue; }
    if (!writeReg(REG_CONFIG, cfg)) { addr_ = 0; continue; }

    cfg_ = cfg;
    out_ = 0xFF;
    return true;
  }
  addr_ = 0;
  return false;
}

bool Pca9557::writeReg(uint8_t reg, uint8_t value) {
  if (addr_ == 0) return false;
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool Pca9557::readReg(uint8_t reg, uint8_t &value) {
  if (addr_ == 0) return false;
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)addr_, 1) != 1) return false;
  value = Wire.read();
  return true;
}

bool Pca9557::reset() {
  bool ok = writeReg(REG_POLARITY, 0x00);  // no inversion
  ok = writeReg(REG_OUTPUT, 0xFF) && ok;   // all outputs high
  ok = writeReg(REG_CONFIG, 0xFF) && ok;   // all pins inputs
  out_ = 0xFF;
  cfg_ = 0xFF;
  return ok;
}

bool Pca9557::pinMode(uint8_t bit, bool input) {
  if (bit > 7) return false;
  uint8_t next = input ? (uint8_t)(cfg_ | (1u << bit)) : (uint8_t)(cfg_ & ~(1u << bit));
  if (next == cfg_) return true;
  if (!writeReg(REG_CONFIG, next)) return false;
  cfg_ = next;
  return true;
}

bool Pca9557::write(uint8_t bit, bool level) {
  if (bit > 7) return false;
  uint8_t next = level ? (uint8_t)(out_ | (1u << bit)) : (uint8_t)(out_ & ~(1u << bit));
  if (next == out_) return true;
  if (!writeReg(REG_OUTPUT, next)) return false;
  out_ = next;
  return true;
}

bool Pca9557::read(uint8_t &levels) { return readReg(REG_INPUT, levels); }

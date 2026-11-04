// =====================================================================
// pca9557.h - minimal driver for the PCA9557 8-bit I2C I/O expander
//
// The CrowPanel uses one expander pin to hold the LCD panel in reset.
// Elecrow's example pulls in a vendor library for this; the chip only has
// four registers, so it is implemented here instead - it also lets the
// driver probe the bus and report the address it actually found, which
// makes a mis-wired board obvious on the serial console.
// =====================================================================
#pragma once

#include <stdint.h>

class Pca9557 {
 public:
  // Returns true if an expander answered at `addr` (or at any address in
  // the PCA9557 range when addr is left at 0).
  bool begin(uint8_t addr = 0);

  // Restore power-on defaults: every pin an input, outputs high.
  bool reset();

  // 1 = input, 0 = output.
  bool pinMode(uint8_t bit, bool input);

  bool write(uint8_t bit, bool level);

  // Read the live pin levels.
  bool read(uint8_t &levels);

  uint8_t address() const { return addr_; }

 private:
  bool writeReg(uint8_t reg, uint8_t value);
  bool readReg(uint8_t reg, uint8_t &value);

  uint8_t addr_ = 0;
  uint8_t out_ = 0xFF;   // output latch
  uint8_t cfg_ = 0xFF;   // 1 = input
};

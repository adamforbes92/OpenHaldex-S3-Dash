#pragma once
// =====================================================================
// ram - keeps the ESP32's own internal RAM free, and reports how it is doing
//
// The chip has two kinds of memory. Internal RAM is fast but small, and WiFi,
// ESP-NOW, the task stacks and the panel's bounce buffer all need it. PSRAM is
// the big 8 MB chip, slower, and the frame buffers live there. Running out of
// internal RAM is what stops WiFi or freezes the dash, while PSRAM has megabytes
// to spare, so anything that does not have to be internal goes to PSRAM.
//
// This file does three things:
//   - sends ordinary memory requests of 512 bytes or more to PSRAM
//   - gives other files a way to ask for PSRAM by name
//   - prints the numbers, and keeps a note if memory ever runs out, so a problem
//     in the car leaves evidence on the serial console
// =====================================================================
#include <Arduino.h>
#include <ArduinoJson.h>
#include <stddef.h>

namespace ram {

/// Call first thing in setup(). Sets the 512-byte rule, starts noting failed
/// memory requests, and prints the note the last boot left behind, if any.
void begin();

/// A zero-filled table from PSRAM, or internal RAM if PSRAM is somehow full.
/// nullptr if there is no memory at all. Used exactly like a normal array.
void *psramCalloc(size_t count, size_t size);

/// The memory source for a JsonDocument: PSRAM first, then internal RAM.
/// Use: JsonDocument doc(ram::jsonAllocator());
ArduinoJson::Allocator *jsonAllocator();

/// One line of memory figures during start-up: "[ram] after <where>: ...".
void printCheckpoint(const char *where);

/// The memory lines of the 5-second report. Call from loop(): it looks through
/// LVGL's memory, and only the task that runs LVGL may do that.
void printStats();

}  // namespace ram

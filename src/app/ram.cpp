// =====================================================================
// ram.cpp - see ram.h. The failed-request note and the JSON memory source are
// the same as can2gauge_RAM.cpp, which found them necessary in the car.
// =====================================================================
#include "app/ram.h"

#include <Arduino.h>
#include <esp_attr.h>        // RTC_NOINIT_ATTR
#include <esp_heap_caps.h>   // the heap_caps_ memory functions
#include <esp_rom_sys.h>     // esp_rom_printf: prints without asking for memory
#include <esp_system.h>      // esp_reset_reason
#include <lvgl.h>
#include <string.h>

#include "net/telemetry.h"

// Ordinary memory requests this size or bigger come from PSRAM first. The
// framework's own limit is 4 KB. can2gauge runs 512 on this chip. Code that has
// to have internal RAM (WiFi, task stacks, the panel's DMA) asks for it by name,
// so it does not move.
#define PSRAM_FROM_BYTES 512  // ** CAN CHANGE THIS **

// Marks the kept note as real: after a power-on that memory holds rubbish.
#define RAM_NOTE_MAGIC 0x0A4D4F48

namespace ram {
namespace {

// What the last failed memory request was.
struct FailNote {
  uint32_t count;    // failed requests since boot
  uint32_t size;     // the last one: bytes asked for
  uint32_t caps;     // the last one: the kind of memory (0x800 = internal)
  uint32_t uptimeS;  // the last one: seconds after boot
  char task[16];     // the last one: the task that asked
};

// ArduinoJson asks for memory through an "Allocator", a small class with three
// functions: take, give back, resize. It has to be a class because that is the
// shape the library expects, and "override" marks each function as our version
// of the library's. Each one asks for PSRAM first, then internal RAM.
struct PsramJsonAllocator : ArduinoJson::Allocator {
  void *allocate(size_t size) override {
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
  }
  void deallocate(void *pointer) override {
    heap_caps_free(pointer);  // works for either kind of memory
  }
  void *reallocate(void *pointer, size_t newSize) override {
    return heap_caps_realloc_prefer(pointer, newSize, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_DEFAULT);
  }
};

// RTC_NOINIT_ATTR: kept in RTC memory, which a reset does not clear (a power cut
// does), so the note from a crash can be read at the next boot.
RTC_NOINIT_ATTR uint32_t g_keptMagic;   // RAM_NOTE_MAGIC when g_keptNote is real
RTC_NOINIT_ATTR FailNote g_keptNote;    // this boot's note, copied as it changes

FailNote g_note = {};                   // this boot's failed requests
PsramJsonAllocator g_jsonAllocator;     // holds nothing itself, so one is enough

// Called by ESP-IDF, in the task that asked, when a memory request cannot be met.
// It must not ask for memory itself, so it uses esp_rom_printf and not Serial.
void onRequestFailed(size_t size, uint32_t caps, const char *functionName) {
  g_note.count++;
  g_note.size = size;
  g_note.caps = caps;
  g_note.uptimeS = millis() / 1000;
  strncpy(g_note.task, pcTaskGetName(NULL), sizeof(g_note.task) - 1);
  g_note.task[sizeof(g_note.task) - 1] = '\0';

  g_keptNote = g_note;  // kept in case this ends in a reset
  g_keptMagic = RAM_NOTE_MAGIC;

  const char *askedBy = "?";
  if (functionName != NULL) askedBy = functionName;
  esp_rom_printf("[ram] OUT OF MEMORY: %u bytes (caps 0x%x, %s) in task %s\n", (unsigned)size,
                 (unsigned)caps, askedBy, g_note.task);
}

}  // namespace

void begin() {
  Serial.println("[ram] Setting up memory rules...");
  heap_caps_malloc_extmem_enable(PSRAM_FROM_BYTES);

  // The note the last boot left. A power-on is skipped because the RTC memory
  // holds rubbish then.
  if (g_keptMagic == RAM_NOTE_MAGIC && esp_reset_reason() != ESP_RST_POWERON && g_keptNote.count > 0) {
    g_keptNote.task[sizeof(g_keptNote.task) - 1] = '\0';  // in case it was half written
    Serial.printf("[ram] LAST BOOT RAN OUT OF MEMORY %u time(s): the last was %u bytes (caps 0x%x) "
                  "in task %s, %u s after boot\n",
                  (unsigned)g_keptNote.count, (unsigned)g_keptNote.size, (unsigned)g_keptNote.caps,
                  g_keptNote.task, (unsigned)g_keptNote.uptimeS);
  }

  // Start this boot's note empty.
  memset(&g_keptNote, 0, sizeof(g_keptNote));
  g_keptMagic = RAM_NOTE_MAGIC;
  heap_caps_register_failed_alloc_callback(onRequestFailed);

  Serial.println("[ram] Set up memory rules!");
  printCheckpoint("memory rules");
}

void *psramCalloc(size_t count, size_t size) {
  return heap_caps_calloc_prefer(count, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

ArduinoJson::Allocator *jsonAllocator() { return &g_jsonAllocator; }

void printCheckpoint(const char *where) {
  Serial.printf("[ram] after %s: internal %u B free (biggest piece %u B), psram %u B free\n", where,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void printStats() {
  // "lowest" is the number that matters: how close it has come to running out
  // since boot. "biggest piece" matters because a request has to fit in one piece.
  // Stack spare is the least each task's stack has ever had free, which says
  // how far that stack could safely be cut.
  Serial.printf("[ram] internal %u B free, lowest %u B, biggest piece %u B | stack spare: ui %u B, net %u B\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)uxTaskGetStackHighWaterMark(NULL),
                (unsigned)telemetry::netStackSpareBytes());

  // LVGL's own memory, holding every widget on every page. It does not exist
  // when the panel failed to start (running headless).
  if (lv_is_initialized()) {
    lv_mem_monitor_t pool;
    lv_mem_monitor(&pool);
    Serial.printf("[ram] lvgl pool %u%% of %u B used, peak %u B, biggest piece %u B\n",
                  (unsigned)pool.used_pct, (unsigned)pool.total_size, (unsigned)pool.max_used,
                  (unsigned)pool.free_biggest_size);
  }

  if (g_note.count > 0) {
    Serial.printf("[ram] OUT OF MEMORY %u time(s) this boot: the last was %u bytes (caps 0x%x) in "
                  "task %s, %u s after boot\n",
                  (unsigned)g_note.count, (unsigned)g_note.size, (unsigned)g_note.caps, g_note.task,
                  (unsigned)g_note.uptimeS);
  }
}

}  // namespace ram

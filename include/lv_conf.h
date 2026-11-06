/**
 * @file lv_conf.h
 * LVGL v9 configuration for the OpenHaldex S3 dash.
 *
 * Deliberately partial: LVGL's lv_conf_internal.h fills in a default for
 * every symbol that is not defined here, so this file only lists the
 * settings that differ from stock.
 *
 * Selected by -DLV_CONF_INCLUDE_SIMPLE (see platformio.ini).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOUR / RENDERING
 *====================*/
/* The RGB panel is wired as RGB565 and LovyanGFX hands LVGL its own
   16-bit frame buffers, so LVGL must render RGB565 too. */
#define LV_COLOR_DEPTH 16

/* 800 px * 2 bytes = 1600 B per line, already 4-byte aligned; keep LVGL
   from choosing a stride that the LCD DMA engine cannot scan. */
#define LV_DRAW_BUF_STRIDE_ALIGN 4

/*====================
   MEMORY
 *====================*/
/* ** CAN CHANGE THIS ** Where LVGL keeps its memory: every widget on every page,
   plus the temporary buffers it draws text through.
   1 = a 256 KB pool in PSRAM (0.5.1). Hands 96 KB of internal RAM back to WiFi and
       the tasks. can2gauge moved its pool for the same reason (2026-10-14), and
       later grew it because 96 KB ran out (0.31.0).
   0 = the old 96 KB pool in internal RAM. Widgets are a little faster to reach, so
       go back to this if [stats] render fps drops noticeably on the glass.
   [ram] lvgl pool on the serial console shows how much of it is really in use. */
#define DASH_LVGL_POOL_IN_PSRAM 1

/* LVGL's own memory manager, so its use of memory stays predictable. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#if DASH_LVGL_POOL_IN_PSRAM
#define LV_MEM_SIZE (256U * 1024U)
#define LV_MEM_POOL_INCLUDE <esp_heap_caps.h>  /* for heap_caps_malloc below */
/* LVGL calls this once, in lv_init(), to get its pool. The panel has already taken
   its frame buffers from PSRAM by then, so PSRAM is known to be working. */
#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define LV_MEM_SIZE (96U * 1024U)
#endif

/*====================
   HAL / TICK
 *====================*/
/* lv_tick_set_cb(millis) is installed in board_display.cpp instead, so the
   HAL tick source here is unused. */
#define LV_USE_OS LV_OS_NONE

/*====================
   LOGGING
 *====================*/
#define LV_USE_LOG 0

/*====================
   FONTS
 *====================*/
/* Only the Montserrat sizes the dashboard actually uses are compiled in;
   every extra size costs flash. */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* The two big numbers (src/ui/fonts) are RLE-compressed: without this,
   lv_font_get_glyph_bitmap() returns NULL for every glyph of a compressed
   font and the letter drawing code dereferences it. That is a hard fault,
   not a fallback, which is exactly what it did. */
#define LV_USE_FONT_COMPRESSED 1

/*====================
   WIDGETS
 *====================*/
#define LV_USE_ARC 1
#define LV_USE_LINE 1
#define LV_USE_LABEL 1
#define LV_USE_BAR 1
#define LV_USE_BUTTON 1
#define LV_USE_TEXTAREA 1
#define LV_USE_KEYBOARD 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* Not used by this UI - each one costs flash. */
#define LV_USE_TABLE 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_SPAN 0
#define LV_USE_OBJ_ID 0

/*====================
   TIMING
 *====================*/
/* LVGL's own refresh timer. It only redraws when something has been
   invalidated, so this is a ceiling rather than a guaranteed frame rate. */
#define LV_DEF_REFR_PERIOD 16

#endif /* LV_CONF_H */

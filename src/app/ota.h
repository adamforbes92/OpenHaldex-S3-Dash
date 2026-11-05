#pragma once
// =====================================================================
// ota - updating the dash where there is no USB cable
//
// Two separate jobs, kept separate on purpose:
//
//  1. A way in. A small HTTP server on the dash's own address. The dash is
//     already a station on the controller's access point, so a phone on that
//     access point can reach it with a browser: no access point of our own, no
//     SD card, and no pushing a 1.6 MB image through ESP-NOW.
//
//  2. A way back. Anything written over the air is a *trial* until the dash
//     proves it works. The outgoing image records that the next boot is on
//     trial before it reboots; the incoming image stands the trial down only
//     once it has come up and run for a while. A crash loop, a dead panel or
//     an image that never gets that far puts the previous slot back instead of
//     leaving a dead dash behind the dashboard.
//
// The trial lives in NVS under its own namespace rather than in the settings
// blob: it has to survive independently of the config version, and it is
// written by the *old* image on the way out.
//
// On the mechanism: esp-idf's own rollback
// (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) is the natural home for this, but
// enabling it here would mean custom_sdkconfig, which forces a full framework
// rebuild against the shared core directory - and that rebuild does not
// currently succeed for this platform release. So the trial is ours, in the
// application. It cannot catch a crash that happens before this code runs -
// the bootloader's version could - but it does catch a crash loop and a boot
// that comes up useless, and it asks nothing of the platform.
// =====================================================================
#include <stddef.h>
#include <stdint.h>

namespace ota {

/// Read the trial state, and fall back to the previous slot if this image has
/// already failed too many times. Call early in setup(): this is the one place
/// a bad image hands the car back.
void begin();

/// Drive from the main loop. Stands a trial down once the panel has come up
/// and stayed up; gives up on it if that never happens. Does nothing at all
/// when no trial is armed.
void tick();

/// Start the update page and mDNS. WiFi must be up: called from the network
/// task once the station has an address.
void serveBegin();

/// Pump the HTTP server and act on a finished upload. Non-blocking when idle;
/// called from the network task.
void servePoll();

/// True while an upload is being written.
bool busy();

/// Bytes written so far in this upload (0 when idle).
size_t written();

/// True while this image is still on trial.
bool onTrial();

/// The slot the running image came from: "ota_0", "ota_1" or "factory".
const char *runningSlot();

/// The address to browse to, or "--" before the link is up.
const char *url();

/// One line for the settings page and the console: version, slot, trial state.
const char *stateLine();

/// Bench aid: arm a trial on the *next* boot and make it fail on purpose, so
/// the fall-back path can be exercised without a bad firmware. Refused unless
/// the other slot actually holds an image, because the point is to test the
/// recovery, not to prove the bootloader can survive an empty slot.
bool armFailureTest(uint32_t failAfterMs);

}  // namespace ota

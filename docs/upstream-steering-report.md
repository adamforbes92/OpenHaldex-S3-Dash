# Upstream report: `steeringAngle` is withheld for Gen1 in `/api/dashboard`

Ready to paste into an issue on
[Forbes-Automotive/OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6).
Line numbers are against the source as downloaded, `FW_VERSION "11.00.4"` —
the same version the reference car runs.

---

## Title

`/api/dashboard` withholds `steeringAngle` on Gen1, but the same value is
published over ESP-NOW and BLE

## Body

**What I see.** On a Gen1 (PQ) car, at the same instant:

```
  field          ESP-NOW         HTTP
  coolant             --        30.00 C  (HTTP only)     <- HTTP is up and healthy
  oil                 --        18.00 C
  mode                 1            1
  haldexState          8            8
  chassisCAN         yes          yes
  steering          1.00           -- deg
```

Every other field agrees, so the HTTP link is fine — the steering angle is
simply being withheld there. It is live on the packet: it has read 1.00 and
5.00 deg in different sessions, so the sensor is being decoded and updated.

`steeringHealthy` is also `false` on Gen1, so a client that checks the health
flag is actively misinformed rather than just given no value.

**Why it looks like an oversight rather than a limitation.** Three code paths
publish the same `received_steering_angle`, with three different rules:

| Path | Rule | Gen1? |
|---|---|---|
| CAN decode | none — it decodes the frames | yes |
| ESP-NOW status packet | none — fresh data is published | yes |
| BLE | every gen whose chassis bus carries `LW_1`/`LWI_01`, minus GM/Ford and Volvo/Saab | yes |
| `/api/dashboard` | allow-list: `2, 4, 50, 52` | **no** |

The CAN side decodes **PQ `LW_1` (0x0C2)** as well as MQB `LWI_01` (0x086)
(`src/OpenHaldexC6_can.cpp:959-980`) — `LW_1` is the PQ frame, and PQ is where
Gen1 cars live. So the decoder goes out of its way to support exactly the
platform whose generation is then excluded by the API.

More tellingly, the BLE path carries a comment saying the API's rule is the
odd one:

```c
// Unlike the web view this is not limited to the gens that use steering for lock
// scaling: every gen whose chassis bus carries LW_1 / LWI_01 reports it. GM/Ford
// (41/42) are excluded because foreign frames on those IDs would be misdecoded.
static bool bleSteeringDeci(int16_t &out)
{
  const bool fresh = hasCANChassis && received_steering_ms != 0 &&
                     (millis() - received_steering_ms) <= steeringStaleMs &&
                     haldexGeneration != 41 && haldexGeneration != 42 &&
                     !isVolvoOrSaabGeneration(haldexGeneration);
```

— `src/OpenHaldexC6_BLE.cpp:269-280`. The generation list in the API
(`src/OpenHaldexC6_API.cpp:233-243`) is a *lock-scaling* list: not every gen
that can report an angle needs one for lock scaling, but that is a different
question from whether the angle exists.

**Impact.** It is visible in the controller's own dashboard, not just to
third-party consumers: `data/app.js:600` renders `data.steeringAngle`, so the
web page shows `--` on a car that is sending an angle 10 times a second. Any
HTTP-only client (a phone, a logger, a display like mine if it did not also
listen on ESP-NOW) is blind to it.

**Suggested fix.** Drop the allow-list and keep the freshness test, which is
what the BLE path already does:

```c
const bool steerHealthy = chassisOk && received_steering_ms != 0 &&
                          (millis() - received_steering_ms) <= steeringStaleMs;
```

Absence is already handled honestly: if `LW_1`/`LWI_01` never arrives,
`received_steering_ms` stays 0 and the field is `null` anyway. If an explicit
blacklist is wanted for the same reason BLE has one, exclude only 41/42 and
`isVolvoOrSaabGeneration(...)` rather than allow-listing the rest.

**Two smaller things, same area** (no opinion on whether they are deliberate):

1. The API reports **magnitude only** (`fabsf(...)`), while BLE reports the
   signed angle using `received_steering_negative` / `LW1_LRW_Sign`. If the web
   view wants magnitude that is fine, but the API field is named
   `steeringAngle` and a consumer cannot tell.
2. `steeringStaleMs` is 500 ms in `include/OpenHaldexC6_defs.h:795` and is used
   by all three paths, so freshness is consistent — worth keeping if the gate
   moves.

**Environment.** OpenHaldex-C6, `FW_VERSION "11.00.4"`, `haldexGeneration` 1,
chassis CAN up, `espNowControl` on. Client: OpenHaldex-S3-Dash (a display that
reads both transports and compares them field by field, which is how this
turned up).

---

## Note for us (not part of the report)

Our dash is unaffected in practice — it prefers the ESP-NOW packet, which
carries the angle, and the raw telemetry page shows both side by side with
`--` where the API withholds it. So this is a report, not a blocker. It is in
`TODO.md` §4.4; the row added to `printCompare()` (`e` on the console) is what
produced the evidence above, and it stays useful for the next disagreement.

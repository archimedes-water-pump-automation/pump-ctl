# MQTT event contract

The four modules of this system talk to each other over MQTT only. This file
is the single description of what they send and what they expect to receive.

It is mirrored, unchanged, in all four repositories:

- [`tank-node`](https://github.com/archimedes-water-pump-automation/tank-node)
- [`pump-ctl`](https://github.com/archimedes-water-pump-automation/pump-ctl)
- [`scheduled-valve`](https://github.com/archimedes-water-pump-automation/scheduled-valve)
- [`archimedes-server`](https://github.com/archimedes-water-pump-automation/archimedes-server)

Changing a payload means changing this file and every repository that
publishes or subscribes to the topic in question, in the same change.

## The envelope

Every message on every topic is one JSON object. Four fields are common to
all of them, and always come first:

| Field | Type | Required | Meaning |
|---|---|---|---|
| `event` | string | yes | `"level"`, `"pump"`, `"valve"` or `"command"`. Identifies the payload shape below. |
| `device` | string | yes | Id of the publisher (`"tank-01"`, `"pump-01"`, `"activator-01"`). This is also the primary key `archimedes-server` stores the event against, so a device id must match the `id` of its row in `archimedes.water_tank` / `archimedes.pump`. |
| `timestamp` | string | no | When the publisher observed the event, RFC 3339 in UTC (`"2026-09-05T03:10:12Z"`). |
| `uptime_s` | number | no | Seconds since the publisher booted. |

Then the fields of the event itself.

### Rules that hold on every topic

- **Unknown fields are ignored.** Adding a field is backwards compatible;
  renaming, removing, or retyping one is not.
- **`timestamp` is optional because a clock can be missing, never because it
  is unimportant.** The boards have no battery-backed RTC, so they omit it
  until SNTP has landed, and a last will carries none at all — the broker
  publishes it on the device's behalf, long after the device wrote it.
  A consumer that stores time falls back to the moment it received the
  message. `uptime_s` is absent in last wills for the same reason.
- **Units live in the field name.** `distance_cm` is centimetres,
  `flow_lpm` is litres per minute. No message carries a bare `distance`.
- **A field's type never varies** except where this file says it is
  nullable: `distance_cm` is a number or `null`, never a string, and never
  absent when the event declares it.
- **`null` is not zero.** `distance_cm: null` means "no reading". A consumer
  must never treat it as a distance of 0 cm, which reads as a full tank.

## Topics

### `watertank/tank-01/level`

Published by `tank-node` every `LEVEL_PUBLISH_MS` (5 s).
Subscribed by `pump-ctl` (as a control input) and `archimedes-server`.

**QoS 0, not retained.** A retained or queued level reading is by definition
old, but it arrives the instant a subscriber connects, so arrival-time
freshness would score it as current.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `valid` | boolean | yes | Whether `distance_cm` is a reading at all. |
| `distance_cm` | number \| null | yes | Distance from the sensor face down to the water surface. *Decreases* as the tank fills. `null` whenever `valid` is `false`. |
| `reason` | string | when `valid` is `false` | `"sensor_unreadable"` (fewer than 3 good pings) or `"node_offline"` (last will). |

```json
{"event":"level","device":"tank-01","timestamp":"2026-09-05T03:10:12Z",
 "valid":true,"distance_cm":62.5,"uptime_s":360}
```

```json
{"event":"level","device":"tank-01","timestamp":"2026-09-05T03:10:12Z",
 "valid":false,"distance_cm":null,"reason":"sensor_unreadable","uptime_s":360}
```

Last will, published by the broker if the node drops off uncleanly:

```json
{"event":"level","device":"tank-01","valid":false,"distance_cm":null,
 "reason":"node_offline"}
```

`pump-ctl` rejects a reading whose `event` is not `level`, whose `device` is
not the tank it is configured to follow, whose `valid` is false, or whose
`distance_cm` falls outside `DIST_MIN_VALID_CM`–`DIST_MAX_VALID_CM`, and
treats every rejection as a sensor fault rather than as room in the tank.
It derives "tank full" from this stream (`distance_cm <= DIST_FULL_CM`);
there is no separate full-tank event.

`archimedes-server` converts `distance_cm` to a volume through the tank's
stored shape and writes it against `device`. **The `dimensions` stored for a
tank must therefore be in centimetres too**, since the calculator works in
whatever unit it is given. Readings with `valid: false` are recorded as
nothing at all — the server skips them rather than storing a volume it
cannot compute.

### `watertank/pump-01/pump`

Published by `pump-ctl`, once per pump transition.
Subscribed by `archimedes-server`.

**QoS 1, retained.** This is a state topic: a dashboard connecting late
should learn what the pump is doing now.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `state` | string | yes | `"on"`, `"off"`, or `"unknown"` (last will only). |
| `reason` | string | yes | What caused the transition. `"flow_confirmed"` for a start; `"tank_full"`, `"pipeline_dry"`, `"max_runtime"`, `"sensor_fault"`, `"lockout_expired"`, `"sensor_recovered"` or `"boot"` for a stop; `"controller_offline"` in the last will. |
| `flow_lpm` | number | no | Pipeline inflow at the moment of the transition. Absent in the last will. |
| `distance_cm` | number \| null | no | Last known tank level, `null` when no valid level was available. Absent in the last will. |

```json
{"event":"pump","device":"pump-01","timestamp":"2026-09-05T03:10:12Z",
 "state":"on","reason":"flow_confirmed","flow_lpm":11.40,
 "distance_cm":62.5,"uptime_s":338}
```

Last will:

```json
{"event":"pump","device":"pump-01","state":"unknown",
 "reason":"controller_offline"}
```

`archimedes-server` maps `state: "on"` to the start of a pump run and
`state: "off"` to the end of one, storing `reason` as the run's stop reason.
`state: "unknown"` is logged and ignored: the last will says the controller
is unreachable, not that the pump stopped, and inventing a stop time for it
would put a fabricated run in the history.

### `watertank/activator-01/cmd`

Published by `pump-ctl`. Subscribed by `scheduled-valve`.

**QoS 1, never retained.** Each command authorises one specific moment. A
retained `keep_open` would replay on every reconnect and hold a mains valve
open with no trial behind it; a retained `turn_off` would shut a trial that
had only just started. `scheduled-valve` rejects retained messages on this
topic for the same reason.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `command` | string | yes | `"keep_open"` or `"turn_off"`. |
| `reason` | string | no | The pump transition behind the command: `"flow_confirmed"` for `keep_open`, `"tank_full"` or `"pipeline_dry"` for `turn_off`. Logged by the activator; it never changes what the command does. |

```json
{"event":"command","device":"pump-01","timestamp":"2026-09-05T03:10:12Z",
 "command":"keep_open","reason":"flow_confirmed","uptime_s":338}
```

```json
{"event":"command","device":"pump-01","timestamp":"2026-09-05T03:14:41Z",
 "command":"turn_off","reason":"tank_full","uptime_s":607}
```

`scheduled-valve` also accepts a bare command word (`keep_open`) on this
topic, which is a convenience for `mosquitto_pub` during bring-up and not
part of what any module publishes.

`keep_open` is sent **before** the pump's relay closes and `turn_off`
**after** it opens: the valve must be held open before the pump draws on the
pipeline, and must not shut while it still is. Both are best-effort — a lost
`keep_open` ends in a trial timeout and the pump's own dry cutoff, a lost
`turn_off` in the activator's `MAX_HOLD_MS` guard.

### `watertank/activator-01/valve`

Published by `scheduled-valve`, once per valve transition. No module in this
system subscribes to it today; it exists for dashboards and for diagnosing
what the activator did.

**QoS 1, retained** — a state topic, like the pump one.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `state` | string | yes | `"open"`, `"closed"`, or `"unknown"` (last will only). |
| `reason` | string | yes | `"scheduled_trial"`, `"keep_open"`, `"trial_timeout"`, `"turn_off"`, `"max_hold"`, `"boot"`, or `"controller_offline"` in the last will. |
| `local_time` | string | no | `"HH:MM"` on the activator's own clock, `"--:--"` before SNTP lands. Human-facing; the machine-readable time is `timestamp`. |

```json
{"event":"valve","device":"activator-01","timestamp":"2026-09-05T06:10:00Z",
 "state":"open","reason":"scheduled_trial","local_time":"03:10","uptime_s":1840}
```

Last will:

```json
{"event":"valve","device":"activator-01","state":"unknown",
 "reason":"controller_offline"}
```

## Flow through the system

```
scheduled-valve ──opens valve on schedule (trial)──▶ pipeline
        ▲                                              │
        │ watertank/activator-01/cmd                    ▼
        │ {"command":"keep_open"} / {"command":"turn_off"}
        │                                          pump-ctl
        └──────────────────────────────────────────┘  │
                                                      │ watertank/pump-01/pump
                                                      ▼ {"state":"on"|"off"}
   tank-node ──watertank/tank-01/level──▶ pump-ctl     archimedes-server
             {"valid":true,"distance_cm":62.5}         (PostgreSQL)
             └──────────────────────────────────▶ archimedes-server
```

1. `scheduled-valve` opens the supply valve on its schedule and starts a
   trial window.
2. Water reaches the pipeline; `pump-ctl`'s flow sensor confirms inflow, so
   it publishes `keep_open`, starts the pump, and publishes `state: "on"`.
3. `tank-node` publishes the level every 5 s. `pump-ctl` uses it to decide
   the tank is full; `archimedes-server` stores it as a volume.
4. When the tank fills or the pipeline runs dry, `pump-ctl` stops the pump,
   publishes `state: "off"` with the reason, and publishes `turn_off` so the
   activator shuts the valve.

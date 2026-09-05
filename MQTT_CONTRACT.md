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
- **A measurement and a decision are different messages.** The raw distance
  goes to the server, which stores it; the state derived from it goes to the
  pump controller, which acts on it. Each consumer receives what it is
  entitled to act on, and the thresholds in between have exactly one home.

## Topics

### `watertank/tank-01/level`

Published by `tank-node` every `LEVEL_PUBLISH_MS` (5 s).
Subscribed by `archimedes-server`, and by nothing else.

**QoS 0, not retained.** A retained or queued level reading is by definition
old, but it arrives the instant a subscriber connects, so arrival-time
freshness would score it as current.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `valid` | boolean | yes | Whether `distance_cm` is a reading at all. |
| `distance_cm` | number \| null | yes | Distance from the sensor face down to the water surface. *Decreases* as the tank fills. `null` whenever `valid` is `false`. |
| `reason` | string | when `valid` is `false` | `"sensor_unreadable"` (fewer than 3 good pings). |

```json
{"event":"level","device":"tank-01","timestamp":"2026-09-05T03:10:12Z",
 "valid":true,"distance_cm":62.5,"uptime_s":360}
```

```json
{"event":"level","device":"tank-01","timestamp":"2026-09-05T03:10:12Z",
 "valid":false,"distance_cm":null,"reason":"sensor_unreadable","uptime_s":360}
```

`archimedes-server` converts `distance_cm` to a volume through the tank's
stored shape and writes it against `device`. **The `dimensions` stored for a
tank must therefore be in centimetres too**, since the calculator works in
whatever unit it is given. Readings with `valid: false` are recorded as
nothing at all — the server skips them rather than storing a volume it
cannot compute.

This topic carries a measurement and nothing else. Nothing controls anything
from it: the decision that a distance means the tank is full belongs to the
node holding the sensor, and travels on the topic below.

### `watertank/tank-01/full_tank`

Published by `tank-node` every `LEVEL_PUBLISH_MS` (5 s), from the same
reading that produced the level message.
Subscribed by `pump-ctl`, and by nothing else.

**QoS 0, not retained**, for the same reason as the level stream, and one
more: this stream holds a pump off, so a late message treated as current is
a tank that overflows.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `state` | string | yes | `"full"`, `"not_full"`, or `"unknown"`. |
| `reason` | string | when `state` is `"unknown"` | `"sensor_unreadable"` (fewer than 3 good pings) or `"node_offline"` (last will). |

```json
{"event":"full_tank","device":"tank-01","timestamp":"2026-09-05T03:10:12Z",
 "state":"not_full","uptime_s":360}
```

Last will, published by the broker if the node drops off uncleanly:

```json
{"event":"full_tank","device":"tank-01","state":"unknown",
 "reason":"node_offline"}
```

**There is no distance on this topic, deliberately.** The threshold that turns
a distance into a state — `DIST_FULL_CM` — lives in `tank-node`'s config,
beside the sensor that produces the distance. A controller that also saw the
raw reading could apply a second copy of that threshold, free to drift from
the one actually deciding, with no way to tell which copy had drifted.

**This topic can only ever stop the pump.** Nothing about the tank starts it:
`pump-ctl` starts on confirmed inflow at its flow sensor and on nothing else,
so a level that has dropped is not a state anyone reports or acts on. That is
why the tank answers one question rather than reporting where the water is.

| State | Means | Effect on the pump |
|---|---|---|
| `full` | `distance_cm <= DIST_FULL_CM`: nowhere left to put water | Running: stops, with reason `tank_full`, and releases the supply valve. Idle: blocks a start that flow would otherwise make |
| `not_full` | There is room | Nothing on its own. A start still needs confirmed inflow |
| `unknown` | The node could not read its sensor, or has dropped off | Fault: the pump is held off |

`full` blocking a start is not the tank starting or gating the pump on a
level: flow is still the only trigger, and the block only avoids closing the
relay onto a tank that would stop it again on the next control cycle.

`unknown` is never room in the tank, and neither is silence. `pump-ctl` faults
after `TANK_FAULT_LIMIT` consecutive cycles without a usable state, and a
message older than `TANK_STALE_MS` (20 s, four missed publishes) is not a
usable state. Publishing every cycle rather than only on a change is what
makes that silence detectable. A node that has gone quiet cannot report a full
tank, which is the one thing that stops a running pump in time.

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
| `tank_state` | string | no | The tank node's last word on the tank when the relay moved — one of the three states above. Carried for diagnosis; absent in the last will. `pump-ctl` never sees a distance, so it cannot report one. |

```json
{"event":"pump","device":"pump-01","timestamp":"2026-09-05T03:10:12Z",
 "state":"on","reason":"flow_confirmed","flow_lpm":11.40,
 "tank_state":"not_full","uptime_s":338}
```

Last will:

```json
{"event":"pump","device":"pump-01","state":"unknown",
 "reason":"controller_offline"}
```

On its first connection after a restart, `pump-ctl` publishes the state its
relay is actually in with `reason: "boot"`, correcting a retained message left
over from before the restart and closing a run the server had open when the
controller died. Only the first connection: a reconnect mid-run would
republish a start that already happened.

`archimedes-server` maps `state: "on"` to the start of a pump run and
`state: "off"` to the end of one, storing `reason` as the run's stop reason.
A `"off"` for a pump with no open run records nothing rather than rewriting
the last completed run.

**`archimedes-server` ignores retained messages**, on this topic and every
other. Retention exists for dashboards: the server connects with a clean
session, so the broker replays the retained pump event on every reconnect, and
processing it again would record a second run for a start that happened once.
The cost is that a server starting mid-run does not learn the pump is running
until its next transition.
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

On its first connection after a restart, `scheduled-valve` publishes the state
its relay is actually in with `reason: "boot"` — a reboot while holding leaves
the valve closed, because the relay de-energises on power loss, and the
retained message would otherwise still read `"open"`.

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
        └──────────────────────────────────────────┘  │   ▲
                                                      │   │ watertank/tank-01/full_tank
                       watertank/pump-01/pump         │   │ {"state":"full"|"not_full"|…}
                       {"state":"on"|"off"}           ▼   │
                                            archimedes-server
                                              (PostgreSQL)
                                                      ▲
   tank-node ──┬──watertank/tank-01/level─────────────┘
               │  {"valid":true,"distance_cm":62.5}
               └──watertank/tank-01/full_tank──▶ pump-ctl
                  {"state":"full"|"not_full"}
```

1. `scheduled-valve` opens the supply valve on its schedule and starts a
   trial window.
2. Water reaches the pipeline. `pump-ctl`'s flow sensor confirms inflow —
   the only thing that starts its pump — so it publishes `keep_open`, starts
   the pump, and publishes `state: "on"`. A tank the node last called `full`
   is the one thing that holds that start back.
3. `tank-node` takes a reading every 5 s and publishes it twice: the distance
   to `archimedes-server`, which stores it as a volume, and the state derived
   from it to `pump-ctl`, which acts on it.
4. The tank reaches `full`, or the pipeline runs dry: those two stop the
   pump. `pump-ctl` publishes `state: "off"` with the reason and `turn_off`
   so the activator shuts the valve. A tank node that goes quiet stops it
   too, as a fail-safe rather than as a control input — silence cannot
   report a full tank.

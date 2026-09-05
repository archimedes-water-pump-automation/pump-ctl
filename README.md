# pump-controller

ESP32 firmware that switches a water pump based on pipeline inflow and tank
level, and reports state over MQTT.

The pump is started by the flow sensor and by nothing else. The tank level is
**not** measured locally and is not received here at all:
[`tank-node`](https://github.com/archimedes-water-pump-automation/tank-node)
measures it, decides whether the tank is full, and sends this controller that
decision — `full`, `not_full` or `unknown` — over MQTT, where it can only ever
stop the pump. See [MQTT contract](#mqtt-contract).

## Behaviour

**Starts** on confirmed inflow at the flow sensor, and on nothing else. A
tank that has drained is not a reason to run a pump — water arriving in the
pipeline is, and only the flow sensor sees that.

**Stops** when the tank node reports `full`, or when the pipeline runs dry.
Those are the two control inputs. Two guards can also stop it, and are not
control: a tank node that has gone quiet (it cannot report a full tank, so
the pump must not keep running blind) and `MAX_RUN_MS`.

A tank the node last called `full` blocks a start. That is not the level
starting or gating the pump: flow is still the only trigger, and the block
only avoids closing the relay onto a tank that would stop it again on the
next 500 ms cycle.

Inflow comes from the upstream valve driven by
[`scheduled-valve`](../scheduled-valve), which opens on a schedule and closes
again unless it is told to stay open. So the start transition also publishes a
`keep_open` event for that module, before the relay closes, and a stop on
`tank_full` or `pipeline_dry` publishes `turn_off` once the relay has opened.
See [MQTT contract](#mqtt-contract).

| State | Meaning |
|---|---|
| `idle` | Waiting for confirmed flow |
| `pumping` | Relay closed |
| `lockout` | Just stopped, waiting out the minimum off time |
| `fault` | Tank state unavailable or stale; pump held off |

Stop reasons published on the pump topic: `tank_full`, `pipeline_dry`,
`max_runtime`, `sensor_fault`, `sensor_recovered`, `lockout_expired`.

### Safety properties

These are deliberate and should not be relaxed without understanding why they
are here:

- **Flow is the only start.** No level, threshold or timer starts this pump.
  The tank node's `full` can hold a start back and stop a run; nothing it
  sends can begin one.
- **The fullness threshold lives at the tank node.** `DIST_FULL_CM` is in
  that node's config, beside the sensor it is applied to. A copy here would
  be a second number free to drift from the one actually deciding, with no
  way to tell which had drifted.
- **Dry-run grace period.** The dry cutoff is suppressed for `DRY_GRACE_MS`
  after start, because flow takes time to establish.
- **Fail-safe on tank-state loss.** `unknown`, an unrecognised state, or one
  from another device is never treated as "tank has room". It stops the pump.
- **Stale network data is a fault.** A tank state older than `TANK_STALE_MS`
  counts as no state at all, which is why the node republishes every cycle
  rather than only on a change.
- **Retained messages are rejected.** A retained tank state is by definition
  old, but arrival time would score it as fresh.
- **Anti short-cycling.** `MIN_OFF_MS` between cycles protects the motor. It
  is the only thing that does, now that there is no second level threshold to
  hold a restart back — see [Known gaps](#known-gaps).
- **Telemetry cannot block control.** Publishes use `esp_mqtt_client_enqueue`
  and are no-ops while offline. An unreachable broker costs log lines, not
  pump behaviour.
- **`keep_open` goes out before the relay closes,** but still cannot gate it.
  It is a command to the upstream valve rather than telemetry, so it is
  ordered ahead of the physical action; being an enqueue that no-ops while
  offline, it can neither block nor prevent that action. A lost `keep_open`
  is not a hazard: the valve times out, the pipeline goes dry and the existing
  dry cutoff stops the pump.
- **`turn_off` goes out after the relay opens,** the opposite order and for
  the same reason. The valve feeds the pipeline this pump draws on, so
  shutting it while the pump still runs would run the pump dry. A lost
  `turn_off` is not a hazard either: the valve closes on its own `MAX_HOLD_MS`
  runaway guard.

## Hardware

| Signal | GPIO | Notes |
|---|---|---|
| Flow sensor pulse | 27 | YF-S201, 5 V → 10 kΩ / 20 kΩ divider |
| Relay IN | 33 | Active low, 10 kΩ pull-up to 3V3 |

Board powered at 5 V on VIN. The flow sensor and relay module run from the
same 5 V rail with a common ground. Relay contacts and everything downstream
are mains and stay off the breadboard.

The 10 kΩ pull-up on GPIO33 is not optional: the pin floats through reset and
the bootloader window, and a floating active-low input can close the relay
before firmware exists.

## Build

```sh
cp main/secrets.h.example main/secrets.h   # then edit it
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Requires ESP-IDF v5.x.

## MQTT contract

Defined in [MQTT_CONTRACT.md](MQTT_CONTRACT.md), which is mirrored in every
repository of this system. This controller shares the full_tank topic with
`tank-node`, the pump topic with `archimedes-server`, and the command topic
with `scheduled-valve`; changing a field means changing it on both sides of
that topic.

**Subscribes** to `watertank/tank-01/full_tank` — QoS 0, not retained:

```json
{"event":"full_tank","device":"tank-01","timestamp":"2026-09-05T03:10:12Z",
 "state":"not_full","uptime_s":360}
```

| `state` | Means | Effect here |
|---|---|---|
| `full` | Nowhere left to put water | Stop, reason `tank_full`, release the supply valve; and block a start until it clears |
| `not_full` | There is room | Nothing on its own: a start needs confirmed inflow |
| `unknown` | Sensor unreadable, or the node dropped off | Fault: pump held off |

A message is accepted only if its `event` is `full_tank`, its `device` is the
configured `TANK_ID`, and its `state` is one of the three names above.
Everything else — an unrecognised state, another tank's state, a payload that
is not a full_tank event, silence for longer than `TANK_STALE_MS` — is a
fault, never room in the tank.

**There is no distance on this topic, and this controller subscribes to
nothing else.** The threshold that turns a distance into a state lives in
`tank-node`'s config, beside the sensor that produces the distance. Receiving
the raw reading here as well would mean a second copy of that threshold, free
to drift from the one actually deciding, with no way to tell which copy had
drifted. The distance goes to `archimedes-server`, which stores it.

**Publishes** to `watertank/pump-01/pump` — QoS 1, retained, one message per
pump transition:

```json
{"event":"pump","device":"pump-01","timestamp":"2026-09-05T03:10:12Z",
 "state":"on","reason":"flow_confirmed","flow_lpm":11.40,
 "tank_state":"not_full","uptime_s":338}
```

`state` is `on` or `off`, and `reason` carries the transition that caused it
(`flow_confirmed`, `tank_full`, `pipeline_dry`, `sensor_fault`,
`max_runtime`) —
`archimedes-server` opens a pump run on `on` and closes it on `off`, storing
`reason` as the stop reason. `tank_state` is the tank node's last word when
the relay moved, carried for diagnosis.

On its first connection after a restart the controller publishes the state the
relay is actually in, with `reason: "boot"`. The topic is retained, so without
it a restart leaves the last pre-restart message standing — `"on"`, possibly,
for a pump that is now off — and a run the server opened before the crash has
nothing to close it. Only the first connection: a reconnect mid-run would
republish a start that already happened.

The last will on the same topic sets `"state":"unknown"` so a dashboard cannot
show `on` indefinitely for a controller that has lost power. It carries no
`timestamp` and no `uptime_s`: the broker publishes it long after this
controller wrote it. The server logs it and stores nothing — an unreachable
controller is not a stopped pump, and inventing a stop time would put a
fabricated run in the history.

**Publishes** to `watertank/activator-01/cmd` — QoS 1, **not retained**:

```json
{"event":"command","device":"pump-01","timestamp":"2026-09-05T03:10:12Z",
 "command":"keep_open","reason":"flow_confirmed","uptime_s":338}
```

```json
{"event":"command","device":"pump-01","timestamp":"2026-09-05T03:14:41Z",
 "command":"turn_off","reason":"tank_full","uptime_s":607}
```

Shared with
[`scheduled-valve`](https://github.com/archimedes-water-pump-automation/scheduled-valve),
which owns that topic. That module opens the supply valve on its own schedule
and shuts it again unless a `keep_open` reaches it inside its trial window
(`KEEP_OPEN_WINDOW_MS`, two minutes by default); once held, it stays open
until `turn_off` or its own `MAX_HOLD_MS` guard. It reads the `command` field
and logs the rest.

| Event | Sent when | Ordering |
|---|---|---|
| `keep_open` | `idle → pumping`, inflow confirmed | **before** the relay closes |
| `turn_off` | pump stops with `tank_full` or `pipeline_dry` | **after** the relay opens |

The two orderings are deliberate and opposite: the valve must be held open
before the pump starts drawing on the pipeline, and must not be shut until
after the pump has stopped drawing on it.

`turn_off` is sent only for the two stops that mean the supply is no longer
wanted — the tank has nowhere to put more water, or there is nothing coming
down the pipe. `max_runtime` and `sensor_fault` are this controller's own
limits and it expects to resume, so they leave the valve as it is.

Never publish either retained. Each event authorises one specific moment; a
retained copy replays on every reconnect and would act with nothing behind it.
The activator rejects retained commands for the same reason.

**Timestamps.** Every published event carries a UTC `timestamp` once SNTP has
landed. The board has no battery-backed RTC, so until then the field is simply
absent and the server falls back to its own receipt time — stamping events
1970 would be worse than not stamping them. The clock never gates the relay.

## Configuration

All tunables are in `main/config.h`.

| Constant | Default | Notes |
|---|---|---|
| `FLOW_PULSES_PER_LITRE` | 450.0 | YF-S201 nominal; calibrate per unit |
| `FLOW_MIN_LPM` | 0.8 | Below this the pipeline counts as dry |
| `TANK_STALE_MS` | 20000 | Four missed tank-node publishes |
| `TANK_FAULT_LIMIT` | 5 | Consecutive cycles without a usable state before faulting |
| `TANK_ID` | tank-01 | Tank node followed; checked against each message's `device` |
| `FLOW_CONFIRM_MS` | 3000 | Inflow must hold this long before the pump starts |
| `SNTP_SERVER` | pool.ntp.org | Source of the UTC `timestamp` field |
| `MIN_OFF_MS` | 30000 | Anti short-cycling |
| `MAX_RUN_MS` | 30 min | Runaway cutoff |

`DIST_FULL_CM` is no longer here. It lives in `tank-node`'s config, beside the
sensor whose reading it is applied to. There is no `DIST_REFILL_CM` anywhere
any more: a restart threshold only made sense while a level could start the
pump, and none can.

Widening `TANK_STALE_MS` widens the window in which the tank can overflow
unobserved.

## Calibration

`FLOW_PULSES_PER_LITRE` varies by 10% or more between units. Run water into a
measuring jug, log the raw pulse count, divide.

The level thresholds are calibrated on the tank node now — see its README.

## Expected startup behaviour

No tank state has arrived at boot, so the controller enters `fault` within
~2.5 s, moves to `lockout` when the first message lands, then waits out
`MIN_OFF_MS`.
Roughly 30–40 s from power-on to ready. This is the fail-safe working.

## Known gaps

- **No restart hysteresis.** The pump stops at `DIST_FULL_CM` and, once
  `MIN_OFF_MS` has passed, may start again as soon as the tank reads
  `not_full` — one centimetre below the stop point is enough. Normally this
  never arises: a `tank_full` stop sends `turn_off`, the supply valve shuts,
  the pipeline dries and there is no flow to start on. It bites only if that
  valve stays open anyway — a lost `turn_off`, a stuck solenoid — where the
  pump can cycle once every `MIN_OFF_MS` while the tank sits at its stop
  level. The second threshold that used to prevent this is gone by design,
  because it started the pump from a level. Raise `MIN_OFF_MS` if that failure
  mode matters more than restart latency.
- A pump stop on `max_runtime` or `sensor_fault` sends no `turn_off`, on the
  assumption that pumping resumes shortly. If it does not — a level sensor
  that never recovers, say — the valve holds open until the activator's own
  `MAX_HOLD_MS` guard expires, four hours by default.
- Credentials are compiled in. Move to NVS before deployment.
- Plaintext MQTT. Switch to `mqtts://` with a CA certificate — an
  unauthenticated broker is an unauthenticated switch for a mains pump.
- No OTA update path.

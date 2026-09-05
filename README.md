# pump-controller

ESP32 firmware that switches a water pump based on pipeline inflow and tank
level, and reports state over MQTT.

The tank level is **not** measured locally. It arrives over MQTT from
[`tank-node`](../tank-node). See [MQTT contract](#mqtt-contract).

## Behaviour

The pump starts only when inflow from the pipeline is confirmed and the tank
has room. It stops when the tank is full, when the pipeline goes dry, or when
any safety limit trips.

Inflow comes from the upstream valve driven by
[`scheduled-valve`](../scheduled-valve), which opens on a schedule and closes
again unless it is told to stay open. So the start transition also publishes a
`keep_open` event for that module, before the relay closes. See
[MQTT contract](#mqtt-contract).

| State | Meaning |
|---|---|
| `idle` | Waiting for flow and a low enough tank |
| `pumping` | Relay closed |
| `lockout` | Just stopped, waiting out the minimum off time |
| `fault` | Level unavailable or stale; pump held off |

Stop reasons published on the pump topic: `tank_full`, `pipeline_dry`,
`max_runtime`, `sensor_fault`, `sensor_recovered`, `lockout_expired`.

### Safety properties

These are deliberate and should not be relaxed without understanding why they
are here:

- **Hysteresis.** Stop at `DIST_FULL_CM`, restart only below `DIST_REFILL_CM`.
  A single threshold makes the relay chatter as the water surface moves.
- **Dry-run grace period.** The dry cutoff is suppressed for `DRY_GRACE_MS`
  after start, because flow takes time to establish.
- **Fail-safe on level loss.** An unreadable or stale level is never treated
  as "tank has room". It stops the pump.
- **Stale network data is a fault.** A level older than `LEVEL_STALE_MS`
  counts as no level at all.
- **Retained messages are rejected.** A retained level is by definition old,
  but arrival time would score it as fresh.
- **Anti short-cycling.** `MIN_OFF_MS` between cycles protects the motor.
- **Telemetry cannot block control.** Publishes use `esp_mqtt_client_enqueue`
  and are no-ops while offline. An unreachable broker costs log lines, not
  pump behaviour.
- **`keep_open` goes out before the relay closes,** but still cannot gate it.
  It is a command to the upstream valve rather than telemetry, so it is
  ordered ahead of the physical action; being an enqueue that no-ops while
  offline, it can neither block nor prevent that action. A lost `keep_open`
  is not a hazard: the valve times out, the pipeline goes dry and the existing
  dry cutoff stops the pump.

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

Shared with `tank-node`. Changing either side requires changing both.

**Subscribes** to `watertank/tank-01/level` — QoS 0, not retained:

```json
{"event":"level","device":"tank-01","distance_cm":62.5,
 "valid":true,"uptime_s":360}
```

`distance_cm` is measured from the sensor face downward, so it *decreases* as
the tank fills. `valid:false` (with `distance_cm: null`) means the tank node
could not read its sensor, or the broker delivered its last will.

**Publishes** to `watertank/pump-01/pump` — QoS 1, retained, one message per
pump transition:

```json
{"event":"pump","device":"pump-01","state":"on","reason":"flow_confirmed",
 "flow_lpm":11.40,"distance_cm":62.5,"uptime_s":338}
```

Last will on the same topic sets `"state":"unknown"` so a dashboard cannot
show `on` indefinitely for a controller that has lost power.

**Publishes** to `watertank/activator-01/cmd` — QoS 1, **not retained**:

```json
{"command":"keep_open"}
```

Shared with [`scheduled-valve`](../scheduled-valve), which owns that topic.
That module opens the supply valve on its own schedule and shuts it again
unless a `keep_open` reaches it inside its trial window
(`KEEP_OPEN_WINDOW_MS`, two minutes by default). This controller sends one at
the moment inflow is confirmed and the tank has room — that is, on the
`idle → pumping` transition, **before** the relay closes — so the valve is
already held open when the pump starts drawing on the pipeline.

Never publish it retained. The event authorises one specific trial; a retained
copy replays on every reconnect and would hold a mains valve open with no
trial behind it.

## Configuration

All tunables are in `main/config.h`.

| Constant | Default | Notes |
|---|---|---|
| `DIST_FULL_CM` | 12.0 | Stop threshold. Must clear the transducer blind zone |
| `DIST_REFILL_CM` | 35.0 | Restart threshold |
| `FLOW_PULSES_PER_LITRE` | 450.0 | YF-S201 nominal; calibrate per unit |
| `FLOW_MIN_LPM` | 0.8 | Below this the pipeline counts as dry |
| `LEVEL_STALE_MS` | 20000 | Four missed tank-node publishes |
| `MIN_OFF_MS` | 30000 | Anti short-cycling |
| `MAX_RUN_MS` | 30 min | Runaway cutoff |

Widening `LEVEL_STALE_MS` widens the window in which the tank can overflow
unobserved.

## Calibration

`FLOW_PULSES_PER_LITRE` varies by 10% or more between units. Run water into a
measuring jug, log the raw pulse count, divide.

`DIST_FULL_CM` and `DIST_REFILL_CM` depend on where the transducer is
physically mounted. Measure from the sensor face to the intended stop level
and add margin.

## Expected startup behaviour

No level has arrived at boot, so the controller enters `fault` within ~2.5 s,
moves to `lockout` when the first message lands, then waits out `MIN_OFF_MS`.
Roughly 30–40 s from power-on to ready. This is the fail-safe working.

## Known gaps

- Nothing sends `turn_off` to the activator when the pump stops. The valve
  stays open until its own `MAX_HOLD_MS` runaway guard expires (four hours by
  default). Closing the loop is the obvious next step.
- Credentials are compiled in. Move to NVS before deployment.
- Plaintext MQTT. Switch to `mqtts://` with a CA certificate — an
  unauthenticated broker is an unauthenticated switch for a mains pump.
- No OTA update path.

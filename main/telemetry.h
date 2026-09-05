#pragma once

#include <stdbool.h>

/* What the tank node reports about the tank. This controller receives
 * the state, never the distance behind it: both thresholds live in the
 * tank node's config, with the sensor that produces the reading they
 * are applied to. See MQTT_CONTRACT.md.
 *
 * TANK_PARTIAL is the hysteresis band between them, where a running
 * pump keeps running and an idle one stays idle. */
typedef enum {
    TANK_UNKNOWN,    /* no usable reading; never means room in the tank */
    TANK_FULL,       /* nowhere to put water: stop pumping             */
    TANK_PARTIAL,    /* inside the hysteresis band: no transition      */
    TANK_REFILLABLE  /* low enough to start again                      */
} tank_state_t;

void telemetry_start(void);
bool telemetry_online(void);

/* Published once per pump state change, retained, in the envelope
 * MQTT_CONTRACT.md defines for the pump topic. tank_state is the tank
 * node's last word on the tank, carried for diagnosis: it says what
 * this controller was acting on when it moved the relay. */
void telemetry_publish_pump(bool on,
                            const char *reason,
                            float flow_lpm,
                            tank_state_t tank_state);

/* Asks the upstream activator to hold its supply valve open.
 *
 * Sent when the flow sensor confirms inflow, before the pump starts.
 * Like every publish here it enqueues and is a no-op while offline,
 * so it can never gate or delay the relay.
 *
 * reason is the pump transition behind the command. The activator logs
 * it; it never changes what the command does. */
void telemetry_publish_keep_open(const char *reason);

/* Releases that hold, so the activator shuts its supply valve.
 *
 * Sent after the pump has stopped because the tank is full or the
 * pipeline has gone dry. Never sent before the relay opens: the valve
 * feeds the pipeline this pump draws on. reason carries which of the
 * two stops it was. */
void telemetry_publish_turn_off(const char *reason);

/* Latest tank state received over MQTT.
 *
 * Writes TANK_UNKNOWN and returns false when nothing has arrived, the
 * tank node reported the state unknown, or the last message is older
 * than TANK_STALE_MS. The caller must treat a false return as a fault,
 * never as "tank still has room". */
bool telemetry_tank_state_get(tank_state_t *out_state);

/* The wire name of a state, for logging and for the pump event. */
const char *tank_state_name(tank_state_t state);

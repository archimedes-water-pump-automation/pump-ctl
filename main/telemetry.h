#pragma once

#include <stdbool.h>

void telemetry_start(void);
bool telemetry_online(void);

/* Published once per pump state change, retained, in the envelope
 * MQTT_CONTRACT.md defines for the pump topic. Pass
 * distance_valid=false when no usable level was available: the reading
 * then goes out as null rather than as a distance of zero, which the
 * server would read as a full tank. */
void telemetry_publish_pump(bool on,
                            const char *reason,
                            float flow_lpm,
                            float distance_cm,
                            bool distance_valid);

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

/* Latest tank level received over MQTT.
 *
 * Returns false when no reading has arrived, the last one was flagged
 * invalid, or it is older than LEVEL_STALE_MS. The caller must treat a
 * false return as a sensor fault, never as "tank still has room". */
bool telemetry_level_get(float *out_cm);

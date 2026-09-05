#pragma once

#include <stdbool.h>

void telemetry_start(void);
bool telemetry_online(void);

/* Published once per pump state change, retained. */
void telemetry_publish_pump(bool on,
                            const char *reason,
                            float flow_lpm,
                            float distance_cm,
                            bool distance_valid);

/* Asks the upstream activator to hold its supply valve open.
 *
 * Sent when the flow sensor confirms inflow, before the pump starts.
 * Like every publish here it enqueues and is a no-op while offline,
 * so it can never gate or delay the relay. */
void telemetry_publish_keep_open(void);

/* Releases that hold, so the activator shuts its supply valve.
 *
 * Sent after the pump has stopped because the tank is full or the
 * pipeline has gone dry. Never sent before the relay opens: the valve
 * feeds the pipeline this pump draws on. */
void telemetry_publish_turn_off(void);

/* Latest tank level received over MQTT.
 *
 * Returns false when no reading has arrived, the last one was flagged
 * invalid, or it is older than LEVEL_STALE_MS. The caller must treat a
 * false return as a sensor fault, never as "tank still has room". */
bool telemetry_level_get(float *out_cm);

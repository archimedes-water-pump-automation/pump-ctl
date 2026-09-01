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

/* Latest tank level received over MQTT.
 *
 * Returns false when no reading has arrived, the last one was flagged
 * invalid, or it is older than LEVEL_STALE_MS. The caller must treat a
 * false return as a sensor fault, never as "tank still has room". */
bool telemetry_level_get(float *out_cm);

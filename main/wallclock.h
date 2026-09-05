#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Buffer size for wallclock_iso8601(): "2026-09-05T03:10:12Z" plus NUL. */
#define WALLCLOCK_ISO8601_LEN 21

/* Starts SNTP. Returns immediately and the clock becomes valid
 * asynchronously; nothing here ever waits on it. Call after the network
 * is up. */
void wallclock_start(void);

/* Writes the current UTC time as RFC 3339, the form MQTT_CONTRACT.md
 * requires of the envelope's timestamp field.
 *
 * Returns false while the clock is still unsynced, which is why that
 * field is optional: a board with no battery-backed RTC would otherwise
 * stamp every event 1970 and the server would store it. */
bool wallclock_iso8601(char *buf, size_t len);

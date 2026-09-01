#pragma once

/* Configures GPIO, installs the sensor ISRs and starts the control task.
 * Safe to call before or after telemetry_start(). */
void control_start(void);

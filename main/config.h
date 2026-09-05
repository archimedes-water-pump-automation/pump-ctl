#pragma once

/* ======================= identity ======================= */

#define DEVICE_ID "pump-01"

/* ======================= pin map ======================= */

#define PIN_FLOW   GPIO_NUM_27   /* YF-S201 pulse, level shifted */
#define PIN_RELAY  GPIO_NUM_33   /* relay module IN              */

/* The ultrasonic sensor no longer connects here. It lives on the
 * tank node and arrives over MQTT. */

#define RELAY_ACTIVE_LOW 1

/* ======================= tank geometry ======================= */

#define DIST_FULL_CM      12.0f   /* <= this: tank is full, stop pumping */
#define DIST_REFILL_CM    35.0f   /* >= this: low enough to start again  */
#define DIST_MIN_VALID_CM  3.0f   /* sanity check on received values     */
#define DIST_MAX_VALID_CM 400.0f

/* ======================= flow ======================= */

#define FLOW_PULSES_PER_LITRE 450.0f
#define FLOW_MIN_LPM            0.8f

/* ======================= timing ======================= */

#define CONTROL_PERIOD_MS      500
#define DRY_GRACE_MS          8000
#define DRY_CONFIRM_MS        4000
#define FLOW_CONFIRM_MS       3000
#define MIN_OFF_MS           30000
#define MAX_RUN_MS      (30 * 60 * 1000)
#define LEVEL_FAULT_LIMIT        5

/* The tank node publishes every 5 s. Four missed messages is a link
 * that is no longer trustworthy, so the pump stops. Widening this
 * widens the window in which the tank can overflow unobserved. */
#define LEVEL_STALE_MS       20000

/* ======================= network ======================= */

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy main/secrets.h.example to main/secrets.h and fill in credentials"
#endif

#define TOPIC_PUMP  "watertank/" DEVICE_ID "/pump"
#define TOPIC_LEVEL "watertank/tank-01/level"   /* subscribed, not published */

/* ======================= upstream activator ======================= */

/* The scheduled-valve module opens the supply valve on its own
 * schedule and shuts it again unless a keep_open event reaches it
 * inside its trial window. This controller sends that event once
 * inflow is confirmed, so the valve keeps feeding the pipeline the
 * pump is about to draw on.
 *
 * Its command topic is the only thing shared with that module. The
 * event is published QoS 1 and never retained: it authorises one
 * specific trial, and a retained copy would replay on every reconnect
 * and hold a mains valve open with no trial behind it. */
#define ACTIVATOR_ID "activator-01"

#define TOPIC_ACTIVATOR_CMD "watertank/" ACTIVATOR_ID "/cmd"   /* published */

#pragma once

/* ======================= identity ======================= */

#define DEVICE_ID "pump-01"

/* ======================= pin map ======================= */

#define PIN_FLOW   GPIO_NUM_27   /* YF-S201 pulse, level shifted */
#define PIN_RELAY  GPIO_NUM_33   /* relay module IN              */

/* The ultrasonic sensor no longer connects here, and neither does its
 * reading: the tank node measures the distance and decides what it
 * means. This controller receives that decision, not the centimetres
 * behind it. */

#define RELAY_ACTIVE_LOW 1

/* No tank geometry here any more. DIST_FULL_CM, DIST_REFILL_CM and the
 * validity range moved to the tank node's config.h, next to the sensor
 * that produces the distance they are applied to. A threshold kept here
 * as well would be a second copy, free to drift from the one actually
 * deciding, with no way to tell which had drifted. */

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
#define TANK_FAULT_LIMIT         5

/* The tank node publishes every 5 s. Four missed messages is a link
 * that is no longer trustworthy, so the pump stops. Widening this
 * widens the window in which the tank can overflow unobserved. */
#define TANK_STALE_MS        20000

/* ======================= clock ======================= */

/* Pump events carry a UTC timestamp once SNTP has landed; see
 * MQTT_CONTRACT.md. Until then the field is absent and the server falls
 * back to its own receipt time. The clock never gates the relay. */
#define SNTP_SERVER "pool.ntp.org"

/* ======================= network ======================= */

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy main/secrets.h.example to main/secrets.h and fill in credentials"
#endif

/* The tank node this controller follows. Its id is checked against the
 * device field of every message on the topic below: the topic says
 * where a message arrived, the envelope says who sent it, and another
 * tank's state is not this tank's state. */
#define TANK_ID "tank-01"

#define TOPIC_PUMP "watertank/" DEVICE_ID "/pump"

/* The tank node's derived state: full, partial, refillable, unknown.
 * Not its level. The distance stays between that node and the server;
 * this controller is told what the tank is, not what it measures. */
#define TOPIC_FULL_TANK "watertank/" TANK_ID "/full_tank"  /* subscribed */

/* ======================= upstream activator ======================= */

/* The scheduled-valve module opens the supply valve on its own
 * schedule and shuts it again unless a keep_open event reaches it
 * inside its trial window. This controller sends that event once
 * inflow is confirmed, so the valve keeps feeding the pipeline the
 * pump is about to draw on.
 *
 * The reverse event, turn_off, releases that hold once the tank is
 * full or the pipeline has gone dry, rather than leaving the valve to
 * sit open until the activator's own four-hour runaway guard.
 *
 * Its command topic is the only thing shared with that module. Both
 * events are published QoS 1 and never retained: each authorises one
 * specific moment, and a retained copy would replay on every reconnect
 * and act with nothing behind it. */
#define ACTIVATOR_ID "activator-01"

#define TOPIC_ACTIVATOR_CMD "watertank/" ACTIVATOR_ID "/cmd"   /* published */

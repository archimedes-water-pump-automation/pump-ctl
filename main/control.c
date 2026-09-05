#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "config.h"
#include "control.h"
#include "telemetry.h"

static const char *TAG = "pump";

/* ======================= flow sensor ======================= */

static volatile uint32_t s_flow_pulses = 0;

static void IRAM_ATTR flow_isr(void *arg)
{
    (void)arg;
    s_flow_pulses++;
}

/* Litres per minute since the previous call. Call at a steady rate. */
static float flow_read_lpm(void)
{
    static int64_t  last_us    = 0;
    static uint32_t last_count = 0;

    int64_t  now   = esp_timer_get_time();
    uint32_t count = s_flow_pulses;

    if (last_us == 0) {
        last_us    = now;
        last_count = count;
        return 0.0f;
    }

    float    dt_s  = (float)(now - last_us) / 1e6f;
    uint32_t delta = count - last_count;   /* wraps correctly on overflow */

    last_us    = now;
    last_count = count;

    if (dt_s <= 0.0f) {
        return 0.0f;
    }
    return (delta / FLOW_PULSES_PER_LITRE) / (dt_s / 60.0f);
}

/* ======================= relay ======================= */

static void relay_write(bool on)
{
    int level = RELAY_ACTIVE_LOW ? !on : on;
    gpio_set_level(PIN_RELAY, level);
}

/* ======================= state machine ======================= */

typedef enum {
    ST_IDLE,      /* waiting for flow and a low enough tank */
    ST_PUMPING,
    ST_LOCKOUT,   /* just stopped, cooling down             */
    ST_FAULT      /* tank state unusable, pump held off     */
} pump_state_t;

static const char *state_name(pump_state_t s)
{
    switch (s) {
        case ST_IDLE:    return "idle";
        case ST_PUMPING: return "pumping";
        case ST_LOCKOUT: return "lockout";
        default:         return "fault";
    }
}

static void control_task(void *arg)
{
    (void)arg;

    pump_state_t state    = ST_IDLE;
    bool         pump_on  = false;
    const char  *reason   = "boot";

    int64_t state_since_ms = esp_timer_get_time() / 1000;
    int64_t flow_ok_since  = 0;   /* 0 == not currently flowing */
    int64_t dry_since      = 0;   /* 0 == not currently dry     */
    int     tank_faults    = 0;

    for (;;) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        float lpm     = flow_read_lpm();
        bool  flowing = lpm >= FLOW_MIN_LPM;

        /* The tank node's own verdict on the tank: full, not full, or
         * nothing usable. The threshold behind it lives over there,
         * with the sensor. It can only ever stop this pump. */
        tank_state_t tank    = TANK_UNKNOWN;
        bool         tank_ok = telemetry_tank_state_get(&tank);

        if (tank_ok) {
            tank_faults = 0;
        } else if (tank_faults < TANK_FAULT_LIMIT) {
            tank_faults++;
        }

        if (flowing) {
            if (flow_ok_since == 0) flow_ok_since = now_ms;
            dry_since = 0;
        } else {
            if (dry_since == 0) dry_since = now_ms;
            flow_ok_since = 0;
        }

        pump_state_t next = state;

        /* Set alongside a stop reason that means the pipeline is no
         * longer wanted open. The other stops are this controller's own
         * limits and it expects to resume shortly, so they leave the
         * supply where it is. */
        bool release_supply = false;

        switch (state) {

        case ST_IDLE:
            /* Confirmed inflow is the only thing that starts this pump.
             * The tank level does not start it and never did anything
             * but stop it: water arriving in the pipeline is the whole
             * reason to run, and a tank that has drained is not, on its
             * own, water to pump.
             *
             * The tank still gets a veto. A tank the node last called
             * full has nowhere to put what the pipeline is delivering,
             * so starting into it would close the relay and open it
             * again on the next cycle with tank_full. Blocking is not
             * starting: flow remains the only trigger.
             *
             * A state that is merely stale does not block a start —
             * flow is the trigger, and a link that stays quiet trips
             * the fault below within TANK_FAULT_LIMIT cycles anyway. */
            if (tank_faults >= TANK_FAULT_LIMIT) {
                next   = ST_FAULT;
                reason = "sensor_fault";
            } else if (flow_ok_since != 0 &&
                       (now_ms - flow_ok_since) >= FLOW_CONFIRM_MS &&
                       tank != TANK_FULL) {
                next   = ST_PUMPING;
                reason = "flow_confirmed";
            }
            break;

        case ST_PUMPING:
            /* Two things stop a running pump: the tank filling up, and
             * the pipeline running dry. The rest of this branch is
             * guards, not control — a tank node that has gone quiet
             * cannot tell us the tank filled, and MAX_RUN_MS is a
             * runaway cutoff for a pump that neither ever reports. */
            if (tank_faults >= TANK_FAULT_LIMIT) {
                next   = ST_FAULT;
                reason = "sensor_fault";
            } else if (tank_ok && tank == TANK_FULL) {
                next            = ST_LOCKOUT;
                reason          = "tank_full";
                release_supply  = true;   /* nowhere left to put water */
            } else if ((now_ms - state_since_ms) >= DRY_GRACE_MS &&
                       dry_since != 0 &&
                       (now_ms - dry_since) >= DRY_CONFIRM_MS) {
                next            = ST_LOCKOUT;
                reason          = "pipeline_dry";
                release_supply  = true;   /* nothing coming down the pipe */
            } else if ((now_ms - state_since_ms) >= MAX_RUN_MS) {
                next   = ST_LOCKOUT;
                reason = "max_runtime";
            }
            break;

        case ST_LOCKOUT:
            if ((now_ms - state_since_ms) >= MIN_OFF_MS) {
                next   = ST_IDLE;
                reason = "lockout_expired";
            }
            break;

        case ST_FAULT:
            /* Recovers on its own once the tank node reports again. */
            if (tank_ok) {
                next   = ST_LOCKOUT;
                reason = "sensor_recovered";
            }
            break;
        }

        if (next != state) {
            state          = next;
            state_since_ms = now_ms;
            ESP_LOGI(TAG, "-> %s (%s), tank %s",
                     state_name(state), reason, tank_state_name(tank));

            bool want_pump = (state == ST_PUMPING);
            if (want_pump != pump_on) {
                /* Confirmed inflow means the upstream valve is inside
                 * its trial window and will shut again on its own
                 * unless it hears keep_open. Send it before the relay
                 * closes, so the valve is held open before the pump
                 * starts drawing on the pipeline.
                 *
                 * This does not weaken "the relay moves first": the
                 * publish enqueues and is a no-op while offline, so it
                 * can neither block the control task nor become a
                 * precondition for the physical action. If it is lost,
                 * the valve times out, the pipeline goes dry and the
                 * existing dry cutoff stops the pump. */
                if (want_pump) {
                    telemetry_publish_keep_open(reason);
                }

                /* The relay moves before telemetry. Reporting follows
                 * the physical action, never gates it. */
                pump_on = want_pump;
                relay_write(pump_on);
                telemetry_publish_pump(pump_on, reason, lpm, tank);

                /* Release the valve only once the pump is already off.
                 * It feeds the pipeline this pump draws on, so shutting
                 * it first would run the pump dry. Nothing is lost if
                 * this one goes missing either: the activator closes on
                 * its own runaway guard. */
                if (!want_pump && release_supply) {
                    telemetry_publish_turn_off(reason);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

/* ======================= init ======================= */

void control_start(void)
{
    /* Latch the relay off before the pin becomes an output, so enabling
     * the driver cannot produce a glitch that kicks the pump. */
    gpio_set_level(PIN_RELAY, RELAY_ACTIVE_LOW ? 1 : 0);

    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << PIN_RELAY),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_conf));
    relay_write(false);

    gpio_config_t flow_conf = {
        .pin_bit_mask = (1ULL << PIN_FLOW),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&flow_conf));

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_FLOW, flow_isr, NULL));

    ESP_LOGI(TAG, "controller up, pump off");

    xTaskCreate(control_task, "control", 4096, NULL, 5, NULL);
}

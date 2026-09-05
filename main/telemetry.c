#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "config.h"
#include "telemetry.h"
#include "wallclock.h"

static const char *TAG = "telemetry";

static esp_mqtt_client_handle_t s_client;
static volatile bool            s_connected;

/* Written by the MQTT task, read by the control task. */
static portMUX_TYPE s_level_mux = portMUX_INITIALIZER_UNLOCKED;
static float        s_level_cm;
static bool         s_level_valid;
static int64_t      s_level_rx_ms;

/* Retained on the pump topic if the broker loses us without a clean
 * disconnect. A dashboard must not keep showing "on" for a controller
 * that has been unplugged for an hour, and the server needs the device
 * field to know whose pump this is.
 *
 * It carries neither timestamp nor uptime: the broker publishes it on
 * this controller's behalf, long after the controller wrote it, so both
 * would be lies. MQTT_CONTRACT.md makes them optional for this case. */
static const char *LWT_PAYLOAD =
    "{\"event\":\"pump\",\"device\":\"" DEVICE_ID "\","
    "\"state\":\"unknown\",\"reason\":\"controller_offline\"}";

bool telemetry_online(void)
{
    return s_connected;
}

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* enqueue, not publish: esp_mqtt_client_publish() blocks until the message
 * is handed to the transport, which on a flaky link can park the caller for
 * seconds. The control task cannot afford that. */
static void publish(const char *topic, const char *payload, int qos, int retain)
{
    if (!s_connected || s_client == NULL) {
        return;
    }

    int msg_id = esp_mqtt_client_enqueue(s_client, topic, payload,
                                         (int)strlen(payload),
                                         qos, retain, true);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "enqueue failed for %s", topic);
    }
}

/* Appends to buf at *off, tracking the length the message would have
 * needed so the caller can tell a truncated payload from a whole one. */
static void json_append(char *buf, size_t size, size_t *off,
                        const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*off < size ? buf + *off : NULL,
                      *off < size ? size - *off : 0,
                      fmt, ap);
    va_end(ap);

    if (n > 0) {
        *off += (size_t)n;
    }
}

/* Writes the envelope every message on every topic shares: the event
 * name, this device, and the UTC timestamp when the clock has one. See
 * MQTT_CONTRACT.md. */
static void json_open_envelope(char *buf, size_t size, size_t *off,
                               const char *event)
{
    char ts[WALLCLOCK_ISO8601_LEN];

    json_append(buf, size, off, "{\"event\":\"%s\",\"device\":\"%s\"",
                event, DEVICE_ID);

    if (wallclock_iso8601(ts, sizeof(ts))) {
        json_append(buf, size, off, ",\"timestamp\":\"%s\"", ts);
    }
}

void telemetry_publish_pump(bool on,
                            const char *reason,
                            float flow_lpm,
                            float distance_cm,
                            bool distance_valid)
{
    char   payload[256];
    size_t off = 0;

    json_open_envelope(payload, sizeof(payload), &off, "pump");
    json_append(payload, sizeof(payload), &off,
                ",\"state\":\"%s\",\"reason\":\"%s\",\"flow_lpm\":%.2f",
                on ? "on" : "off", reason, flow_lpm);

    /* null, not the 0.0 the caller passes for an unusable level: zero
     * centimetres is a tank filled to the sensor. */
    if (distance_valid) {
        json_append(payload, sizeof(payload), &off,
                    ",\"distance_cm\":%.1f", distance_cm);
    } else {
        json_append(payload, sizeof(payload), &off, ",\"distance_cm\":null");
    }

    json_append(payload, sizeof(payload), &off, ",\"uptime_s\":%lu}",
                (unsigned long)uptime_s());

    if (off >= sizeof(payload)) {
        ESP_LOGE(TAG, "pump payload truncated at %u bytes, not published",
                 (unsigned)sizeof(payload));
        return;
    }

    ESP_LOGI(TAG, "pump event: %s (%s)", on ? "on" : "off", reason);
    publish(TOPIC_PUMP, payload, 1, 1);
}

/* Both commands share the envelope with everything else this controller
 * publishes; the activator reads the command field and logs the rest.
 * Neither is retained. The activator rejects retained commands anyway:
 * a replayed keep_open would hold the valve open with no trial behind
 * it, and a replayed turn_off would shut a trial that has only just
 * started. */
static void publish_command(const char *command, const char *reason)
{
    char   payload[224];
    size_t off = 0;

    json_open_envelope(payload, sizeof(payload), &off, "command");
    json_append(payload, sizeof(payload), &off,
                ",\"command\":\"%s\",\"reason\":\"%s\"", command, reason);
    json_append(payload, sizeof(payload), &off, ",\"uptime_s\":%lu}",
                (unsigned long)uptime_s());

    if (off >= sizeof(payload)) {
        ESP_LOGE(TAG, "command payload truncated at %u bytes, not published",
                 (unsigned)sizeof(payload));
        return;
    }

    ESP_LOGI(TAG, "%s (%s) -> %s", command, reason, TOPIC_ACTIVATOR_CMD);
    publish(TOPIC_ACTIVATOR_CMD, payload, 1, 0);
}

void telemetry_publish_keep_open(const char *reason)
{
    publish_command("keep_open", reason);
}

void telemetry_publish_turn_off(const char *reason)
{
    publish_command("turn_off", reason);
}

bool telemetry_level_get(float *out_cm)
{
    float   cm;
    bool    valid;
    int64_t rx_ms;

    portENTER_CRITICAL(&s_level_mux);
    cm    = s_level_cm;
    valid = s_level_valid;
    rx_ms = s_level_rx_ms;
    portEXIT_CRITICAL(&s_level_mux);

    if (!valid || rx_ms == 0) {
        return false;
    }

    int64_t age = (esp_timer_get_time() / 1000) - rx_ms;
    if (age > LEVEL_STALE_MS) {
        ESP_LOGW(TAG, "level stale by %lld ms", age);
        return false;
    }

    *out_cm = cm;
    return true;
}

/* Parses one level message. The shape it expects is fixed by
 * MQTT_CONTRACT.md and is what tank-node publishes:
 *
 *   {"event":"level","device":"tank-01","timestamp":"...",
 *    "valid":true,"distance_cm":62.5,"uptime_s":360}
 *
 * Anything that fails a check here is recorded as an invalid level,
 * which the control loop treats as a sensor fault. It is never recorded
 * as room in the tank. */
static void handle_level_message(const char *data, int len)
{
    char buf[256];

    if (len <= 0 || len >= (int)sizeof(buf)) {
        ESP_LOGW(TAG, "level payload size %d rejected", len);
        return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        ESP_LOGW(TAG, "level payload not valid json");
        return;
    }

    const cJSON *event  = cJSON_GetObjectItemCaseSensitive(root, "event");
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    const cJSON *valid  = cJSON_GetObjectItemCaseSensitive(root, "valid");
    const cJSON *dist   = cJSON_GetObjectItemCaseSensitive(root, "distance_cm");

    /* The envelope identifies the message before its fields are read.
     * A payload that is not a level, or is somebody else's level, tells
     * us nothing about this tank. */
    if (!cJSON_IsString(event) || strcmp(event->valuestring, "level") != 0) {
        ESP_LOGW(TAG, "payload on level topic is not a level event");
        cJSON_Delete(root);
        return;
    }
    if (!cJSON_IsString(device) || strcmp(device->valuestring, TANK_ID) != 0) {
        ESP_LOGW(TAG, "level from unexpected device, rejected");
        cJSON_Delete(root);
        return;
    }

    bool  ok = cJSON_IsTrue(valid) && cJSON_IsNumber(dist);
    float cm = ok ? (float)dist->valuedouble : 0.0f;

    /* Trust the network no further than the sensor. A value outside the
     * physical range means a wrong device is publishing here, or the
     * tank node is misconfigured. Either way it is not a level. */
    if (ok && (cm < DIST_MIN_VALID_CM || cm > DIST_MAX_VALID_CM)) {
        ESP_LOGW(TAG, "level %.1f cm out of range, rejected", cm);
        ok = false;
    }

    portENTER_CRITICAL(&s_level_mux);
    s_level_cm    = cm;
    s_level_valid = ok;
    s_level_rx_ms = esp_timer_get_time() / 1000;
    portEXIT_CRITICAL(&s_level_mux);

    if (!ok) {
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
        ESP_LOGW(TAG, "tank node reports level invalid (%s)",
                 cJSON_IsString(reason) ? reason->valuestring : "no reason");
    }

    cJSON_Delete(root);
}

/* ======================= event handlers ======================= */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            esp_mqtt_client_subscribe(s_client, TOPIC_LEVEL, 0);
            ESP_LOGI(TAG, "broker connected, subscribed to %s", TOPIC_LEVEL);
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "broker disconnected");
            break;
        case MQTT_EVENT_DATA:
            /* Ignore fragmented payloads and retained messages. A
             * retained level is by definition old, and arrival time
             * would make it look fresh. */
            if (event->data_len == event->total_data_len &&
                event->current_data_offset == 0 &&
                !event->retain) {
                handle_level_message(event->data, event->data_len);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "mqtt error");
            break;
        default:
            break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg; (void)event_data;

    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "wifi lost, retrying");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "wifi up");
        esp_mqtt_client_start(s_client);
    }
}

/* ======================= init ======================= */

void telemetry_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri                  = MQTT_BROKER_URI,
        .credentials.username                = MQTT_USERNAME,
        .credentials.client_id               = DEVICE_ID,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.last_will.topic             = TOPIC_PUMP,
        .session.last_will.msg               = LWT_PAYLOAD,
        .session.last_will.msg_len           = 0,   /* 0 -> strlen */
        .session.last_will.qos               = 1,
        .session.last_will.retain            = 1,
        .session.keepalive                   = 30,
        .network.reconnect_timeout_ms        = 5000,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "mqtt client init failed, running offline");
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());
}

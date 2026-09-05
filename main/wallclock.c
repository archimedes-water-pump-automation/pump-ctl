#include <stdio.h>
#include <time.h>

#include "esp_sntp.h"
#include "esp_log.h"

#include "config.h"
#include "wallclock.h"

static const char *TAG = "wallclock";

static bool s_announced;

void wallclock_start(void)
{
    /* No timezone is set: every timestamp on the wire is UTC, so the
     * server never has to guess which offset a device meant. */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER);
    esp_sntp_init();

    ESP_LOGI(TAG, "sntp started (%s)", SNTP_SERVER);
}

bool wallclock_iso8601(char *buf, size_t len)
{
    time_t    now;
    struct tm utc;

    if (buf == NULL || len < WALLCLOCK_ISO8601_LEN) {
        return false;
    }

    time(&now);
    gmtime_r(&now, &utc);

    /* An unsynced board comes up in 1970. Anything past 2020 means SNTP
     * has landed. */
    if (utc.tm_year <= (2020 - 1900)) {
        return false;
    }

    if (!s_announced) {
        s_announced = true;
        ESP_LOGI(TAG, "clock synced");
    }

    return strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &utc) > 0;
}

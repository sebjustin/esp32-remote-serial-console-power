#include "power_control.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <inttypes.h>

static const char *TAG = "power_control";
static bool s_busy;

/* ProRock input is assumed active-LOW. Verify this on the actual board
 * before connecting it to the motherboard. Relay defaults OFF. */
static void relay_set(bool on) {
    gpio_set_level(POWER_RELAY_GPIO, on ? 0 : 1);
}

static void pulse_task(void *arg) {
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    relay_set(true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    relay_set(false);
    s_busy = false;
    ESP_LOGI(TAG, "POWER pulse finished (%" PRIu32 " ms)", ms);
    vTaskDelete(NULL);
}

void power_control_init(void) {
    /* Preload the output latch to the inactive level before enabling output mode. */
    gpio_set_level(POWER_RELAY_GPIO, 1);
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << POWER_RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    relay_set(false);

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << POWER_STATUS_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&in));
    s_busy = false;
    ESP_LOGI(TAG, "Relay GPIO%d, PWR_OK GPIO%d initialized", POWER_RELAY_GPIO, POWER_STATUS_GPIO);
}

/* With the proposed PC817 output wiring: LOW means PWR_OK asserted. */
bool power_control_is_on(void) { return gpio_get_level(POWER_STATUS_GPIO) == 0; }
bool power_control_is_busy(void) { return s_busy; }

esp_err_t power_control_pulse(uint32_t duration_ms) {
    if (duration_ms < 100 || duration_ms > 10000) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_busy = true;
    if (xTaskCreate(pulse_task, "power_pulse", 2048, (void *)(uintptr_t)duration_ms, 5, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

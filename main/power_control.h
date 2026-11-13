#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define POWER_RELAY_GPIO 4
#define POWER_STATUS_GPIO 5

void power_control_init(void);
bool power_control_is_on(void);
bool power_control_is_busy(void);
esp_err_t power_control_pulse(uint32_t duration_ms);

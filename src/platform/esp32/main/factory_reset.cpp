#include "factory_reset.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

namespace factory_reset {

static const char *TAG = "factory_reset";

static const gpio_num_t BUTTON_GPIO = (gpio_num_t)CONFIG_SVS_RESET_BUTTON_GPIO;
static const int64_t HOLD_US = (int64_t)CONFIG_SVS_RESET_HOLD_SECONDS * 1000 * 1000;
static const int64_t WARN_US = 2 * 1000 * 1000;  // announce the reset after this long
static const int POLL_MS = 50;

void perform()
{
    ESP_LOGW(TAG, "Erasing all settings");
    // Erasing de-initializes NVS first; nothing may use it after this
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "Rebooting into setup mode");
    esp_restart();
}

// The button pulls the pin low while pressed. Polling also debounces it: the
// reset needs the pin to read low on every sample for the whole hold time.
static void button_task(void *arg)
{
    int64_t pressed_since = 0;
    bool warned = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        bool pressed = gpio_get_level(BUTTON_GPIO) == 0;
        int64_t now = esp_timer_get_time();

        if (!pressed) {
            if (warned) {
                ESP_LOGI(TAG, "Button released, factory reset cancelled");
            }
            pressed_since = 0;
            warned = false;
            continue;
        }
        if (pressed_since == 0) {
            pressed_since = now;
            continue;
        }
        int64_t held = now - pressed_since;
        if (!warned && held >= WARN_US) {
            ESP_LOGW(TAG, "Keep holding BOOT for %d s to erase all settings",
                     CONFIG_SVS_RESET_HOLD_SECONDS);
            warned = true;
        }
        if (held >= HOLD_US) {
            perform();
        }
    }
}

void start_button_monitor()
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BUTTON_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io));

    xTaskCreate(button_task, "reset_btn", 3072, NULL, 3, NULL);
}

}  // namespace factory_reset

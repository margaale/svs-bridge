// SVS Bridge
//
// ESP32-S3 bridge to control an SVS (Scalable Video Switch) remotely:
//  - USB host link to the SVS's CH340 (svs_usb)
//  - WiFi with a captive setup portal (wifi_manager, dns_server)
//  - HTTPS web UI with admin login, OTA firmware updates (web_server, tls_cert, auth)
//  - Factory reset from the web UI or the BOOT button (factory_reset)
//  - SVS firmware updates over USB, from the official repository or an
//    uploaded file (svs_flasher, svs_fw_repo)
//
// Anything typed on the "COM" port console and confirmed with Enter is sent to
// the SVS (e.g. SVS_Change_Input_3).

#include <stdio.h>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/uart.h"

#include "auth.h"
#include "factory_reset.h"
#include "svs_flasher.h"
#include "svs_usb.h"
#include "tls_cert.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "main";

// Reads the console (COM port). Enter sends the line to the SVS.
static void console_task(void *arg)
{
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
    std::string cmd;
    while (true) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (!cmd.empty()) {
                esp_err_t err = svs_usb::send(cmd);
                if (err == ESP_ERR_INVALID_STATE) {
                    printf("(SVS not connected, not sent: %s)\n", cmd.c_str());
                } else if (err == ESP_ERR_NOT_SUPPORTED) {
                    printf("(listen-only mode, not sent: %s)\n", cmd.c_str());
                } else if (err == ESP_ERR_NOT_ALLOWED) {
                    printf("(SVS firmware update in progress, not sent: %s)\n", cmd.c_str());
                } else if (err != ESP_OK) {
                    printf("Send error: %s\n", esp_err_to_name(err));
                }
                cmd.clear();
            }
        } else if (c == 0x08 || c == 0x7F) {
            if (!cmd.empty()) {
                cmd.pop_back();
            }
        } else if (c >= 0x20 && c < 0x7F && cmd.size() < 128) {
            cmd += (char)c;
        }
    }
}

// First boot after an OTA update: keep the new firmware only if it joins the
// WiFi network (with the web servers already up, it can then be updated
// again). Otherwise go back to the previous firmware, which did connect.
static void confirm_update_task(void *arg)
{
    const int64_t deadline = esp_timer_get_time() + CONFIG_SVS_OTA_CONFIRM_TIMEOUT * 1000000LL;
    ESP_LOGW(TAG, "New firmware: waiting up to %d s for WiFi to confirm it",
             CONFIG_SVS_OTA_CONFIRM_TIMEOUT);
    while (!wifi_manager::sta_connected()) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "New firmware never connected to WiFi, rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();  // does not return on success
            esp_restart();  // rollback impossible: at least retry
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ota_mark_app_valid_cancel_rollback());
    ESP_LOGI(TAG, "New firmware confirmed");
    vTaskDelete(NULL);
}

static void init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was full or outdated, erasing it");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

extern "C" void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "SVS Bridge %s (%s %s), running from %s, reset reason %d", app->version,
             app->date, app->time, esp_ota_get_running_partition()->label, (int)esp_reset_reason());

    init_nvs();
    // First, so the settings can be wiped even if something below fails
    factory_reset::start_button_monitor();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    svs_usb::start();
    svs_flasher::init();
    wifi_manager::start();
    ESP_ERROR_CHECK(auth::load());
    ESP_ERROR_CHECK(tls_cert::load());
    web_server::start();

    // After an OTA update the new image boots in "pending verify" state. It is
    // only confirmed once it is reachable again, so a remote bridge cannot be
    // stranded by a bad update; see confirm_update_task(). If it crashes
    // before that, the bootloader rolls back on the next reset.
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        xTaskCreate(confirm_update_task, "ota_confirm", 3072, NULL, 5, NULL);
    }

    printf("\nSVS Bridge ready. Plug the SVS into the native USB port.\n"
           "Type a command and press Enter to send it (e.g. SVS_Input_Up)\n\n");

    xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
}

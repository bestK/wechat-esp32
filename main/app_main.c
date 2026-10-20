#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ancs_client.h"
#include "hid_keyboard.h"
#include "hid_http.h"
#include "reset_button.h"
#include "wifi_portal.h"

static const char *TAG = "app";

// iPhone bundle identifiers we care about. Everything else is still printed,
// just tagged as "other".
#define WECHAT_APP_ID "com.tencent.xin"

static const char *category_name(uint8_t category)
{
    switch (category) {
    case 1:  return "incoming-call";
    case 2:  return "missed-call";
    case 3:  return "voicemail";
    case 4:  return "social";
    case 5:  return "schedule";
    case 6:  return "email";
    case 7:  return "news";
    case 8:  return "health";
    case 9:  return "finance";
    case 10: return "location";
    case 11: return "entertainment";
    default: return "other";
    }
}

static void on_notification(const ancs_notification_t *n, void *ctx)
{
    (void)ctx;

    if (strcmp(n->app_id, WECHAT_APP_ID) != 0) {
        ESP_LOGI(TAG, "ignored: app=%s", n->app_id);
        return;
    }

    ESP_LOGI(TAG, "================ WeChat (%s) ================", category_name(n->category));
    ESP_LOGI(TAG, "  from: %s", n->title[0] ? n->title : "(unknown)");
    ESP_LOGI(TAG, "  text: %s", n->message[0] ? n->message : "(empty)");
    ESP_LOGI(TAG, "=============================================");
}

static esp_err_t bt_stack_init(void)
{
    // BLE only: no Classic BT memory on this chip, but the call is harmless
    // and keeps the stack from reserving BR/EDR buffers.
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        return err;
    }
    return esp_bluedroid_enable();
}

void app_main(void)
{
    ESP_LOGI(TAG, "WeChat ANCS watch booting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Must run before the portal decides whether to use the saved network.
    ESP_ERROR_CHECK(reset_button_init());

    // Three quick RST taps: erase credentials now that NVS is available.
    if (reset_button_factory_reset_requested()) {
        wifi_portal_forget();       // Erases NVS and restarts; does not return.
    }

    ESP_ERROR_CHECK(bt_stack_init());

    // HID first: it registers the GATTS callback and its own GATT apps, and
    // sets the HID appearance. ANCS is a GATTC client and does not touch either.
    ESP_ERROR_CHECK(hid_keyboard_init());
    ESP_ERROR_CHECK(hid_keyboard_start());

    ancs_client_set_name("WeChat-Watch");
    ESP_ERROR_CHECK(ancs_client_init(on_notification, NULL));
    ESP_ERROR_CHECK(ancs_client_start_advertising());

    ESP_LOGI(TAG, "pair from iPhone Settings > Bluetooth, then lock the phone and send a WeChat message");

    reset_button_start_monitor();

    // Wi-Fi portal: open AP first, then the web page joins the LAN.
    esp_err_t wifi_err = wifi_portal_start();
    if (wifi_err == ESP_OK) {
        if (hid_http_start() != ESP_OK) {
            ESP_LOGW(TAG, "HID control endpoints unavailable");
        }
        ESP_LOGI(TAG, "join the \"%s\" hotspot, then open http://192.168.4.1/",
                 WIFI_PORTAL_AP_SSID);
    } else {
        ESP_LOGW(TAG, "wifi portal failed: %s", esp_err_to_name(wifi_err));
    }

    // Nothing else to do: the ANCS, HID and Wi-Fi callbacks drive everything.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive (ancs=%d/%d hid=%d wifi=%s)",
                 ancs_client_connected(), ancs_client_ready(),
                 hid_keyboard_connected(),
                 wifi_portal_sta_connected() ? wifi_portal_sta_ip() : "offline");
    }
}

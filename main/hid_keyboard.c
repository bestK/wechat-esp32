/*
 * BLE HID keyboard on top of the Espressif HID device profile in main/hid/.
 *
 * The profile (hid_device_le_prf.c, esp_hidd_prf_api.c) comes from the ESP-IDF
 * ble_hid_device_demo example, which is licensed Unlicense OR CC0-1.0.
 *
 * Two integration constraints shaped this file:
 *
 *  - esp_hidd_register_callbacks() calls esp_ble_gatts_register_callback()
 *    itself, so this module owns the GATTS callback and nothing else may
 *    register one.
 *  - The HID profile is a GATT *server*; ANCS is a GATT *client* on the same
 *    link. Both can coexist, but the advertised packet has to carry the HID
 *    appearance so iOS offers the keyboard UI.
 */

#include "hid_keyboard.h"

#include <string.h>
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_hidd_prf_api.h"
#include "hid_dev.h"

static const char *TAG = "hid_kbd";

// Gap between key down and key up. Too short and iOS drops the keystroke.
#define KEY_PRESS_MS   8
// Gap between consecutive characters.
#define KEY_GAP_MS     12

static bool s_ready;
static bool s_connected;
static uint16_t s_conn_id;

static void hidd_event_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDD_EVENT_REG_FINISH:
        ESP_LOGI(TAG, "HID profile registered");
        s_ready = param->init_finish.state == ESP_HIDD_INIT_OK;
        break;

    case ESP_HIDD_EVENT_BLE_CONNECT:
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        ESP_LOGI(TAG, "host connected, conn=%u", s_conn_id);
        break;

    case ESP_HIDD_EVENT_BLE_DISCONNECT:
        s_connected = false;
        ESP_LOGI(TAG, "host disconnected");
        break;

    case ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT:
        // Host telling us the Caps Lock / Num Lock state. Nothing to do with
        // it, but logging it confirms output reports reach us.
        ESP_LOGD(TAG, "LED report: 0x%02x", param->led_write.data[0]);
        break;

    default:
        break;
    }
}

esp_err_t hid_keyboard_init(void)
{
    esp_err_t err = esp_hidd_profile_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "profile init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Registers the GATTS callback and the HID + battery GATT apps.
    err = esp_hidd_register_callbacks(hidd_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register callbacks failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "HID keyboard initialised");
    return ESP_OK;
}

esp_err_t hid_keyboard_start(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    // Advertising itself is driven by ancs_client: the two services share one
    // advertisement packet and one GAP callback.
    return ESP_OK;
}

bool hid_keyboard_connected(void)
{
    return s_connected;
}

// HID usage id for a printable ASCII character. The keyboard report carries
// usage ids, not characters, so the host's keyboard layout decides the glyph.
static uint8_t usage_for_ascii(char c, bool *needs_shift)
{
    *needs_shift = false;

    if (c >= 'a' && c <= 'z') {
        return 0x04 + (uint8_t)(c - 'a');
    }
    if (c >= 'A' && c <= 'Z') {
        *needs_shift = true;
        return 0x04 + (uint8_t)(c - 'A');
    }
    if (c >= '1' && c <= '9') {
        return 0x1E + (uint8_t)(c - '1');
    }

    switch (c) {
    case '0': return 0x27;
    case '\n': return 0x28;   // Enter
    case '\r': return 0x28;
    case '\t': return 0x2B;
    case ' ': return 0x2C;
    case '-': return 0x2D;
    case '_': *needs_shift = true; return 0x2D;
    case '=': return 0x2E;
    case '+': *needs_shift = true; return 0x2E;
    case '[': return 0x2F;
    case '{': *needs_shift = true; return 0x2F;
    case ']': return 0x30;
    case '}': *needs_shift = true; return 0x30;
    case '\\': return 0x31;
    case '|': *needs_shift = true; return 0x31;
    case ';': return 0x33;
    case ':': *needs_shift = true; return 0x33;
    case '\'': return 0x34;
    case '"': *needs_shift = true; return 0x34;
    case '`': return 0x35;
    case '~': *needs_shift = true; return 0x35;
    case ',': return 0x36;
    case '<': *needs_shift = true; return 0x36;
    case '.': return 0x37;
    case '>': *needs_shift = true; return 0x37;
    case '/': return 0x38;
    case '?': *needs_shift = true; return 0x38;
    case '!': *needs_shift = true; return 0x1E;
    case '@': *needs_shift = true; return 0x1F;
    case '#': *needs_shift = true; return 0x20;
    case '$': *needs_shift = true; return 0x21;
    case '%': *needs_shift = true; return 0x22;
    case '^': *needs_shift = true; return 0x23;
    case '&': *needs_shift = true; return 0x24;
    case '*': *needs_shift = true; return 0x25;
    case '(': *needs_shift = true; return 0x26;
    case ')': *needs_shift = true; return 0x27;
    default:  return 0;       // Not representable on a US keyboard.
    }
}

static void tap_key(uint8_t usage, bool shift)
{
    if (!usage || !s_connected) {
        return;
    }
    key_mask_t mask = shift ? LEFT_SHIFT_KEY_MASK : 0;
    uint8_t key = usage;

    esp_hidd_send_keyboard_value(s_conn_id, mask, &key, 1);
    vTaskDelay(pdMS_TO_TICKS(KEY_PRESS_MS));
    esp_hidd_send_keyboard_value(s_conn_id, 0, NULL, 0);   // Release.
    vTaskDelay(pdMS_TO_TICKS(KEY_GAP_MS));
}

int hid_keyboard_type(const char *utf8)
{
    if (!utf8) {
        return 0;
    }
    if (!s_connected) {
        ESP_LOGW(TAG, "not connected; nothing typed");
        return 0;
    }

    int typed = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p; ++p) {
        // Skip UTF-8 continuation and lead bytes: HID keycodes cannot express
        // non-ASCII, so Chinese text has to be pasted, not typed.
        if (*p >= 0x80) {
            continue;
        }
        bool shift = false;
        uint8_t usage = usage_for_ascii((char)*p, &shift);
        if (!usage) {
            continue;
        }
        tap_key(usage, shift);
        typed++;
    }
    return typed;
}

int hid_keyboard_send_line(const char *utf8)
{
    int typed = hid_keyboard_type(utf8);
    if (s_connected) {
        tap_key(0x28, false);          // Enter.
        typed++;
    }
    return typed;
}

// ---- Mouse -----------------------------------------------------------------

// Largest per-report step and the gap between reports. Each report is one
// notification; sending faster than the connection interval (20-60 ms, see
// ancs_client.c) only queues them, and a deep queue makes swipes jerky.
#define MOUSE_STEP      20
#define MOUSE_GAP_MS    15

static int clamp_step(int v)
{
    return v > MOUSE_STEP ? MOUSE_STEP : v < -MOUSE_STEP ? -MOUSE_STEP : v;
}

// Move by (dx, dy) in small steps with the given buttons held throughout.
// Returns the number of reports sent.
static int mouse_path(int dx, int dy, uint8_t buttons)
{
    int reports = 0;
    while ((dx || dy) && s_connected) {
        int sx = clamp_step(dx);
        int sy = clamp_step(dy);
        esp_hidd_send_mouse_value(s_conn_id, buttons, (int8_t)sx, (int8_t)sy, 0);
        dx -= sx;
        dy -= sy;
        reports++;
        vTaskDelay(pdMS_TO_TICKS(MOUSE_GAP_MS));
    }
    return reports;
}

int hid_mouse_move(int dx, int dy)
{
    if (!s_connected) {
        return 0;
    }
    return mouse_path(dx, dy, 0);
}

int hid_mouse_click(uint8_t buttons)
{
    if (!s_connected || !buttons) {
        return 0;
    }
    esp_hidd_send_mouse_value(s_conn_id, buttons, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(KEY_PRESS_MS * 5));
    esp_hidd_send_mouse_value(s_conn_id, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(MOUSE_GAP_MS));
    return 2;
}

int hid_mouse_swipe(int dx, int dy)
{
    if (!s_connected) {
        return 0;
    }
    // Press, drag, release. iOS needs the press to settle before movement,
    // otherwise the gesture is read as a pointer move rather than a drag.
    esp_hidd_send_mouse_value(s_conn_id, HID_BTN_LEFT, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(MOUSE_GAP_MS * 3));
    int reports = 1 + mouse_path(dx, dy, HID_BTN_LEFT);
    vTaskDelay(pdMS_TO_TICKS(MOUSE_GAP_MS * 3));
    esp_hidd_send_mouse_value(s_conn_id, 0, 0, 0, 0);
    return reports + 1;
}

int hid_mouse_scroll(int clicks)
{
    int reports = 0;
    while (clicks && s_connected) {
        int8_t step = clicks > 0 ? 1 : -1;
        esp_hidd_send_mouse_value(s_conn_id, 0, 0, 0, step);
        clicks -= step;
        reports++;
        vTaskDelay(pdMS_TO_TICKS(MOUSE_GAP_MS * 2));
    }
    return reports;
}

// ---- Consumer (media) keys -------------------------------------------------

int hid_media_key(uint16_t bits)
{
    if (!s_connected || !bits) {
        return 0;
    }
    esp_hidd_send_consumer_bits(s_conn_id, bits);
    vTaskDelay(pdMS_TO_TICKS(KEY_PRESS_MS * 5));
    esp_hidd_send_consumer_bits(s_conn_id, 0);
    vTaskDelay(pdMS_TO_TICKS(KEY_GAP_MS));
    return 1;
}

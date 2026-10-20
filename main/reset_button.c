#include "reset_button.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_portal.h"

static const char *TAG = "reset";

// BOOT is pulled up on the board and reads 0 while pressed.
#define PRESSED 0

// RST (EN) is a hardware reset: the chip restarts before any firmware runs, so
// the press itself cannot be timed or even observed. Instead, count resets that
// happen close together. RTC_NOINIT memory survives a reset without touching
// flash, which matters because writing NVS on every boot would wear it out.
#define RST_MAGIC          0x52535431u     // "RST1"
#define RST_TAP_WINDOW_MS  5000
#define RST_TAPS_TO_RESET  3

typedef struct {
    uint32_t magic;
    uint32_t taps;          // Resets seen in the current burst.
    uint32_t last_ms;       // RTC time of the previous reset.
} rst_state_t;

static RTC_NOINIT_ATTR rst_state_t s_rst;

static bool s_held_at_boot;
static bool s_factory_reset;

static bool is_pressed(void)
{
    return gpio_get_level(RESET_BUTTON_GPIO) == PRESSED;
}

// Reads the RTC clock directly: esp_timer is not running yet, and this value
// only needs to be comparable across resets.
static uint32_t rtc_ms(void)
{
    return (uint32_t)(esp_clk_rtc_time() / 1000);
}

// Returns true when this boot is the Nth rapid reset, i.e. the user is tapping
// RST to ask for a factory reset.
static bool count_reset_tap(void)
{
    if (s_rst.magic != RST_MAGIC) {
        // First boot after flashing or a power cycle: start a fresh burst.
        s_rst.magic = RST_MAGIC;
        s_rst.taps = 1;
        s_rst.last_ms = rtc_ms();
        return false;
    }

    uint32_t now = rtc_ms();
    if ((uint32_t)(now - s_rst.last_ms) <= RST_TAP_WINDOW_MS) {
        s_rst.taps++;
    } else {
        s_rst.taps = 1;            // Too slow: this is a new burst.
    }
    s_rst.last_ms = now;

    if (s_rst.taps >= RST_TAPS_TO_RESET) {
        // Invalidate the whole state, not just zero the counter: the erase ends
        // in esp_restart(), and a surviving magic value would make that restart
        // look like another tap and loop forever.
        s_rst.magic = 0;
        s_rst.taps = 0;
        return true;
    }
    return false;
}

esp_err_t reset_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RESET_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio config failed: %s", esp_err_to_name(err));
        return err;
    }

    // Sample immediately: the portal needs this before it decides whether to
    // connect to the saved network.
    vTaskDelay(pdMS_TO_TICKS(20));      // Let the pull-up settle.
    s_held_at_boot = is_pressed();
    if (s_held_at_boot) {
        ESP_LOGW(TAG, "BOOT held at startup: forcing setup portal");
    }

    // Record the verdict but do not act on it yet: nvs_flash_init() and the
    // Wi-Fi stack are not up at this point, and wifi_portal_forget() needs both.
    s_factory_reset = count_reset_tap();
    if (s_factory_reset) {
        ESP_LOGW(TAG, "%d quick resets detected: will erase credentials",
                 RST_TAPS_TO_RESET);
    }

    return ESP_OK;
}

bool reset_button_factory_reset_requested(void)
{
    return s_factory_reset;
}

bool reset_button_held_at_boot(void)
{
    return s_held_at_boot;
}

static void monitor_task(void *arg)
{
    (void)arg;
    int held_ms = 0;

    while (1) {
        if (is_pressed()) {
            held_ms += 100;
            if (held_ms == 1000) {
                ESP_LOGW(TAG, "keep holding to erase Wi-Fi credentials...");
            }
            if (held_ms >= RESET_HOLD_MS) {
                ESP_LOGW(TAG, "erasing credentials and restarting");
                wifi_portal_forget();      // Does not return.
            }
        } else if (held_ms > 0) {
            if (held_ms >= 1000) {
                ESP_LOGI(TAG, "released after %d ms, cancelled", held_ms);
            }
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void reset_button_start_monitor(void)
{
    xTaskCreate(monitor_task, "reset_btn", 2560, NULL, 3, NULL);
}

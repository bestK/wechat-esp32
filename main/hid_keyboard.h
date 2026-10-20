#ifndef HID_KEYBOARD_H
#define HID_KEYBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the BLE HID keyboard. Must be called after the Bluedroid stack is
// enabled, and before hid_keyboard_start().
esp_err_t hid_keyboard_init(void);

// Register the HID services and start advertising them.
esp_err_t hid_keyboard_start(void);

// True while an iOS device is connected to the HID service.
bool hid_keyboard_connected(void);

// Type a UTF-8 string. Non-ASCII characters are skipped: the keyboard emits
// HID keycodes only, so there is no way to enter Chinese text this way.
// Returns the number of characters actually typed.
int hid_keyboard_type(const char *utf8);

// Type a string followed by Enter.
int hid_keyboard_send_line(const char *utf8);

#ifdef __cplusplus
}
#endif

#endif

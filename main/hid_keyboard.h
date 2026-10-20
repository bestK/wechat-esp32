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

// Mouse buttons for hid_mouse_click().
#define HID_BTN_LEFT    0x01
#define HID_BTN_RIGHT   0x02
#define HID_BTN_MIDDLE  0x04

// The mouse is relative: the host decides where the pointer is, so absolute
// positioning means pushing it into a corner first (a large negative move).
// iOS only shows and honours the pointer with AssistiveTouch enabled.
// All return the number of HID reports sent (0 when not connected).
int hid_mouse_move(int dx, int dy);
int hid_mouse_click(uint8_t buttons);

// Press the left button, drag by (dx, dy), release: a one-finger swipe.
int hid_mouse_swipe(int dx, int dy);

// Wheel clicks; positive scrolls up.
int hid_mouse_scroll(int clicks);

// Tap consumer keys; bits are HID_CC_* from esp_hidd_prf_api.h.
int hid_media_key(uint16_t bits);

#ifdef __cplusplus
}
#endif

#endif

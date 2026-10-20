#ifndef HID_HTTP_H
#define HID_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Control endpoints for the HID keyboard, served on the provisioning portal's
// HTTP server (port 80, shared with the provisioning portal).
//
//   GET  /hid/type?text=hello   type ASCII text, no Enter
//   GET  /hid/line?text=hello   type ASCII text, then Enter
//   GET  /hid/status            JSON: connection state
//
// Non-ASCII input is rejected rather than silently dropped: HID keycodes
// cannot express it, and a partial result would be misleading.

esp_err_t hid_http_start(void);

#ifdef __cplusplus
}
#endif

#endif

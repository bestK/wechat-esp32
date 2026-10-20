#ifndef ANCS_CLIENT_H
#define ANCS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// A notification the iPhone pushed to us. Pointers are valid only for the
// duration of the callback; copy anything you keep.
typedef struct {
    uint32_t uid;
    uint8_t category;      // ANCS CategoryID, e.g. 4 = Social.
    const char *app_id;    // Bundle identifier, e.g. "com.tencent.xin".
    const char *title;     // Sender / subject.
    const char *message;   // Body.
} ancs_notification_t;

// Called from the Bluedroid callback task. Keep it short.
typedef void (*ancs_notify_cb_t)(const ancs_notification_t *n, void *ctx);

// Start the ANCS GATT client. Call once after nvs_flash_init().
esp_err_t ancs_client_init(ancs_notify_cb_t cb, void *ctx);

// Advertising name shown to the iPhone, e.g. "WeChat-Watch".
void ancs_client_set_name(const char *name);

// Start advertising the ANCS service so the iPhone connects to us.
esp_err_t ancs_client_start_advertising(void);

bool ancs_client_connected(void);
// True once the ANCS service has been discovered and subscribed.
bool ancs_client_ready(void);

#ifdef __cplusplus
}
#endif

#endif

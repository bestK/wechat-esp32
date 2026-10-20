#ifndef RESET_BUTTON_H
#define RESET_BUTTON_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Physical recovery path for a device with no screen and no reliable network.
//
// BOOT (GPIO0, the button next to USB on a DevKitC) is active low:
//
//   hold at power-up   -> skip the saved network, force the setup portal
//   hold for 5 s while running -> erase Wi-Fi credentials, restart into setup
//
// The second gesture exists because the web UI is unreachable exactly when it
// is needed most: wrong password, changed router, or a phone that cannot join
// the AP.

#define RESET_BUTTON_GPIO      0
#define RESET_HOLD_MS          5000

// Configure the pin. Call after nvs_flash_init(), before wifi_portal_start().
esp_err_t reset_button_init(void);

// True when the button was held at boot. Consume this once, at startup.
bool reset_button_held_at_boot(void);

// True when three resets landed within 5 s of each other. Checked after
// nvs_flash_init(), since erasing needs NVS up.
bool reset_button_factory_reset_requested(void);

// Start the task that watches for a long press and erases credentials.
void reset_button_start_monitor(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// SoftAP provisioning portal.
//
// Boot sequence:
//   wifi_portal_start()  -> brings up an open AP named "WeChat-Watch"
//   phone/PC joins it    -> http://192.168.4.1 serves a scan + join page
//   credentials saved    -> device joins the LAN and the portal stops
//
// Credentials persist in NVS, so a configured device goes straight to STA on
// the next boot. Hold BOOT during power-up to force the portal back up.

#define WIFI_PORTAL_AP_SSID     "WeChat-Watch"
#define WIFI_PORTAL_AP_PASSWORD ""          // Open network, easier to join.

// Start the SoftAP portal and the HTTP server on 192.168.4.1.
esp_err_t wifi_portal_start(void);

// The portal's HTTP server, so other modules can register endpoints on it.
// Returns NULL before wifi_portal_start() succeeds.
struct httpd_req;
void *wifi_portal_http_server(void);

// True when a scan result list is ready to be served.
bool wifi_portal_scan_ready(void);

// SSID of the network the device is currently joined to, or "" when offline.
const char *wifi_portal_sta_ssid(void);

// IPv4 address of the STA interface as a string, or "" when offline.
const char *wifi_portal_sta_ip(void);

bool wifi_portal_sta_connected(void);

// Clear stored credentials and restart the portal. Exposed over HTTP.
esp_err_t wifi_portal_forget(void);

#ifdef __cplusplus
}
#endif

#endif

/*
 * SoftAP provisioning portal with a self-hosted web UI.
 *
 * Deliberately not using esp_wifi_prov_mgr: that speaks protocomm and needs a
 * companion app, while this needs to be a plain browser page.
 */

#include "wifi_portal.h"

#include <string.h>
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "reset_button.h"

static const char *TAG = "wifi";

#define NVS_NS        "wifi"
#define NVS_SSID      "ssid"
#define NVS_PASS      "pass"

#define MAX_SCAN      20
#define SSID_MAX_LEN  32
#define PASS_MAX_LEN  64

#define STA_MAX_RETRIES 5

static bool s_started;
static int s_sta_retries;
static bool s_scan_done;
static bool s_sta_connected;
static char s_sta_ssid[SSID_MAX_LEN + 1];
static char s_sta_ip[16];

static httpd_handle_t s_server;

// Shared with hid_http.c so both sets of endpoints live on one server (two
// httpd instances would exceed CONFIG_LWIP_MAX_SOCKETS).
void *wifi_portal_http_server(void)
{
    return s_server;
}
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;

typedef struct {
    char ssid[SSID_MAX_LEN + 1];
    int8_t rssi;
    bool secure;
} scan_entry_t;

static scan_entry_t s_scan[MAX_SCAN];
static size_t s_scan_count;

// ---- NVS ------------------------------------------------------------------

static bool load_credentials(char *ssid, char *pass, size_t ssid_len, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t sl = ssid_len;
    size_t pl = pass_len;
    esp_err_t a = nvs_get_str(nvs, NVS_SSID, ssid, &sl);
    esp_err_t b = nvs_get_str(nvs, NVS_PASS, pass, &pl);
    nvs_close(nvs);
    return a == ESP_OK && b == ESP_OK && ssid[0] != '\0';
}

static void save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, NVS_SSID, ssid);
    nvs_set_str(nvs, NVS_PASS, pass);
    nvs_commit(nvs);
    nvs_close(nvs);
}

esp_err_t wifi_portal_forget(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return ESP_FAIL;
    }
    nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "credentials erased, restarting");
    esp_restart();
    return ESP_OK;
}

// ---- event handlers --------------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t count = MAX_SCAN;
        wifi_ap_record_t *recs = calloc(MAX_SCAN, sizeof(wifi_ap_record_t));
        if (!recs) {
            return;
        }
        s_scan_count = 0;
        if (esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
            for (uint16_t i = 0; i < count && s_scan_count < MAX_SCAN; ++i) {
                if (recs[i].ssid[0] == '\0') {
                    continue;                       // Hidden network.
                }
                // Skip duplicates: a mesh or extender can report one SSID twice.
                bool dup = false;
                for (size_t j = 0; j < s_scan_count; ++j) {
                    if (strcmp(s_scan[j].ssid, (const char *)recs[i].ssid) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    continue;
                }
                strlcpy(s_scan[s_scan_count].ssid, (const char *)recs[i].ssid, SSID_MAX_LEN + 1);
                s_scan[s_scan_count].rssi = recs[i].rssi;
                s_scan[s_scan_count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                s_scan_count++;
            }
        }
        free(recs);
        s_scan_done = true;
        ESP_LOGI(TAG, "scan done: %u networks", (unsigned)s_scan_count);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Nothing to do: wifi_portal_start() issues the connect once the saved
        // credentials are configured. Connecting here as well produces
        // "sta is connecting, return error".
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        s_sta_ip[0] = '\0';

        // Retry a few times, then stop. An endless retry loop against a wrong
        // password or an absent router starves the AP: the beacon stops going
        // out and the setup hotspot disappears from scan results.
        if (s_sta_retries < STA_MAX_RETRIES) {
            s_sta_retries++;
            ESP_LOGW(TAG, "STA disconnected (retry %d/%d)", s_sta_retries, STA_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "STA disconnected, giving up; portal stays up on the AP");
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_sta_retries = 0;
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_sta_connected = true;
        ESP_LOGI(TAG, "joined \"%s\", http://%s/", s_sta_ssid, s_sta_ip);
        return;
    }
}

// ---- HTTP ------------------------------------------------------------------

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>WeChat-Watch</title><style>"
    "body{font-family:system-ui,sans-serif;margin:0;padding:24px;background:#0a0e16;color:#e8eef7}"
    "h1{font-size:20px;margin:0 0 4px}p{color:#7e8b9c;font-size:14px;margin:0 0 20px}"
    ".ap{display:block;width:100%;box-sizing:border-box;text-align:left;background:#19202c;"
    "color:#e8eef7;border:1px solid #3c4a5c;border-radius:8px;padding:12px;margin:0 0 8px;"
    "font-size:15px;cursor:pointer;font-family:inherit}"
    ".ap:hover{background:#20c997;color:#0a0e16;border-color:#20c997}"
    "button.ap{appearance:none}"
    ".ap small{display:block;color:#7e8b9c;font-size:12px;margin-top:2px}"
    ".ap:hover small{color:#0a0e16}"
    "input{width:100%;box-sizing:border-box;padding:12px;font-size:15px;border-radius:8px;"
    "border:1px solid #3c4a5c;background:#19202c;color:#e8eef7;margin:0 0 10px}"
    "button{width:100%;padding:12px;font-size:15px;border-radius:8px;border:0;"
    "background:#20c997;color:#0a0e16;font-weight:600;cursor:pointer}"
    "a{color:#7e8b9c;font-size:13px}</style></head><body>";

static esp_err_t root_get(httpd_req_t *req)
{
    char body[4096];
    size_t off = 0;

    off += snprintf(body + off, sizeof(body) - off,
                    "%s<h1>WeChat-Watch</h1><p>%s</p>", PAGE_HEAD,
                    s_sta_connected ? "Connected. Tap a network to change." :
                                      "Pick your Wi-Fi network.");

    if (s_sta_connected) {
        off += snprintf(body + off, sizeof(body) - off,
                        "<p>Joined <b>%s</b><br>http://%s/</p>",
                        s_sta_ssid, s_sta_ip);
    }

    if (!s_scan_done) {
        off += snprintf(body + off, sizeof(body) - off, "<p>Scanning&hellip;</p>");
    } else if (s_scan_count == 0) {
        off += snprintf(body + off, sizeof(body) - off, "<p>No networks found.</p>");
    }

    for (size_t i = 0; i < s_scan_count && off < sizeof(body) - 256; ++i) {
        // The SSID goes into a form value, so it is HTML-escaped here.
        char esc[SSID_MAX_LEN * 6 + 1];
        size_t e = 0;
        for (const char *c = s_scan[i].ssid; *c && e < sizeof(esc) - 8; ++c) {
            if (*c == '"' || *c == '&' || *c == '<' || *c == '>') {
                e += snprintf(esc + e, sizeof(esc) - e, "&#%d;", (int)(unsigned char)*c);
            } else {
                esc[e++] = *c;
            }
        }
        esc[e] = '\0';

        // One form per network: the SSID button reveals the password field, so
        // open networks can still be joined with the field left blank.
        if (s_scan[i].secure) {
            off += snprintf(body + off, sizeof(body) - off,
                            "<form method=post action=/join style='margin:0 0 8px'>"
                            "<input type=hidden name=ssid value=\"%s\">"
                            "<div class=ap onclick=\"this.nextElementSibling.hidden=false;"
                            "this.nextElementSibling.querySelector('input').focus()\">"
                            "%s<small>%d dBm · secured</small></div>"
                            "<div hidden style='padding:0 0 8px'>"
                            "<input type=password name=pass placeholder='Wi-Fi password' autocomplete=off>"
                            "<button type=submit>Connect</button></div>"
                            "</form>",
                            esc, esc, s_scan[i].rssi);
        } else {
            off += snprintf(body + off, sizeof(body) - off,
                            "<form method=post action=/join style='margin:0 0 8px'>"
                            "<input type=hidden name=ssid value=\"%s\">"
                            "<button class='ap' type=submit style='margin:0'>"
                            "%s<small>%d dBm · open</small></button>"
                            "</form>",
                            esc, esc, s_scan[i].rssi);
        }
    }

    off += snprintf(body + off, sizeof(body) - off,
                    "<p style='margin-top:24px'>"
                    "<a href='/scan'>Rescan</a> &nbsp;·&nbsp; "
                    "<a href='/forget'>Reset</a></p></body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, body, off);
}

// Percent-decoding for application/x-www-form-urlencoded bodies.
static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t d = 0;
    for (size_t i = 0; src[i] && d < dst_len - 1; ++i) {
        if (src[i] == '+') {
            dst[d++] = ' ';
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[d++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[d++] = src[i];
        }
    }
    dst[d] = '\0';
}

static bool form_field(const char *body, const char *name, char *out, size_t out_len)
{
    char key[32];
    snprintf(key, sizeof(key), "%s=", name);
    const char *p = strstr(body, key);
    if (!p) {
        return false;
    }
    p += strlen(key);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char raw[PASS_MAX_LEN * 3 + 1];
    if (len >= sizeof(raw)) {
        len = sizeof(raw) - 1;
    }
    memcpy(raw, p, len);
    raw[len] = '\0';
    url_decode(out, raw, out_len);
    return true;
}

static esp_err_t join_post(httpd_req_t *req)
{
    char body[512];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    int got = httpd_req_recv(req, body, total);
    if (got <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    }
    body[got] = '\0';

    char ssid[SSID_MAX_LEN + 1];
    char pass[PASS_MAX_LEN + 1];
    if (!form_field(body, "ssid", ssid, sizeof(ssid))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
    }
    if (!form_field(body, "pass", pass, sizeof(pass))) {
        pass[0] = '\0';
    }

    ESP_LOGI(TAG, "joining \"%s\"", ssid);
    save_credentials(ssid, pass);
    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta http-equiv=refresh content='4;url=/'>"
        "<body style='font-family:system-ui;padding:24px;background:#0a0e16;color:#e8eef7'>"
        "<h1 style='font-size:20px'>Joining...</h1>"
        "<p style='color:#7e8b9c'>The device is connecting. This page refreshes "
        "in a few seconds; check the serial log for the assigned IP.</p></body>");
}

static esp_err_t scan_get(httpd_req_t *req)
{
    s_scan_done = false;
    s_scan_count = 0;
    esp_wifi_scan_start(NULL, false);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

// Erasing is destructive and irreversible over the network, so GET only shows
// a confirmation page and POST does the work.
static esp_err_t forget_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font-family:system-ui;padding:24px;background:#0a0e16;color:#e8eef7'>"
        "<h1 style='font-size:20px'>Forget this network?</h1>"
        "<p style='color:#7e8b9c'>The device restarts into setup mode and "
        "you will need to enter the Wi-Fi password again.</p>"
        "<form method=post action=/forget>"
        "<button style='width:100%;padding:12px;font-size:15px;border-radius:8px;"
        "border:0;background:#ff6b6b;color:#0a0e16;font-weight:600'>Forget and restart</button>"
        "</form>"
        "<p style='margin-top:20px'><a style='color:#7e8b9c' href='/'>&larr; Back</a></p>"
        "</body>");
}

static esp_err_t forget_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><meta charset=utf-8>"
        "<body style='font-family:system-ui;padding:24px;background:#0a0e16;color:#e8eef7'>"
        "<h1 style='font-size:20px'>Forgotten</h1>"
        "<p style='color:#7e8b9c'>Restarting into setup mode.</p></body>");
    vTaskDelay(pdMS_TO_TICKS(500));
    return wifi_portal_forget();
}

// ---- JSON API --------------------------------------------------------------
//
// The form endpoints exist for browsers; these exist for scripts. Kept
// deliberately small: no JSON library, just enough parsing for a flat object
// with string values, which is all this needs.

// Extracts "key":"value" from a flat JSON object. Returns false when absent.
static bool json_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t d = 0;
    while (*p && *p != '"' && d < out_len - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[d++] = '\n'; break;
            case 't': out[d++] = '\t'; break;
            case 'r': out[d++] = '\r'; break;
            case '"': out[d++] = '"';  break;
            case '\\': out[d++] = '\\'; break;
            case 'u':
                // Only ASCII escapes are meaningful here; skip the code point.
                p += 4;
                continue;
            default: out[d++] = *p; break;
            }
            p++;
        } else {
            out[d++] = *p++;
        }
    }
    out[d] = '\0';
    return true;
}

static void send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
}

static esp_err_t api_wifi_post(httpd_req_t *req)
{
    char body[512];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        send_json(req, "400 Bad Request", "{\"error\":\"bad length\"}\n");
        return ESP_OK;
    }
    int got = httpd_req_recv(req, body, total);
    if (got <= 0) {
        send_json(req, "400 Bad Request", "{\"error\":\"no body\"}\n");
        return ESP_OK;
    }
    body[got] = '\0';

    char ssid[SSID_MAX_LEN + 1];
    char pass[PASS_MAX_LEN + 1];
    if (!json_string(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        send_json(req, "400 Bad Request", "{\"error\":\"missing ssid\"}\n");
        return ESP_OK;
    }
    if (!json_string(body, "password", pass, sizeof(pass))) {
        pass[0] = '\0';                     // Open network.
    }

    ESP_LOGI(TAG, "api: joining \"%s\"", ssid);
    save_credentials(ssid, pass);
    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    s_sta_retries = 0;

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();

    send_json(req, "200 OK", "{\"status\":\"connecting\"}\n");
    return ESP_OK;
}

static esp_err_t api_scan_get(httpd_req_t *req)
{
    // Serve the cached result and kick off a fresh scan for the next call.
    char body[2048];
    size_t off = 0;
    off += snprintf(body + off, sizeof(body) - off, "{\"networks\":[");
    for (size_t i = 0; i < s_scan_count && off < sizeof(body) - 128; ++i) {
        // Escape backslash and quote: SSIDs are arbitrary bytes.
        char esc[SSID_MAX_LEN * 2 + 1];
        size_t e = 0;
        for (const char *c = s_scan[i].ssid; *c && e < sizeof(esc) - 3; ++c) {
            if (*c == '"' || *c == '\\') {
                esc[e++] = '\\';
            }
            esc[e++] = *c;
        }
        esc[e] = '\0';
        off += snprintf(body + off, sizeof(body) - off,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                        i ? "," : "", esc, s_scan[i].rssi,
                        s_scan[i].secure ? "true" : "false");
    }
    off += snprintf(body + off, sizeof(body) - off, "],\"scanning\":%s}\n",
                    s_scan_done ? "false" : "true");

    send_json(req, "200 OK", body);

    s_scan_done = false;
    s_scan_count = 0;
    esp_wifi_scan_start(NULL, false);
    return ESP_OK;
}

static esp_err_t api_status_get(httpd_req_t *req)
{
    char body[256];
    snprintf(body, sizeof(body),
             "{\"sta_connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"ap\":\"%s\"}\n",
             s_sta_connected ? "true" : "false",
             s_sta_connected ? s_sta_ssid : "",
             s_sta_connected ? s_sta_ip : "",
             WIFI_PORTAL_AP_SSID);
    send_json(req, "200 OK", body);
    return ESP_OK;
}

static esp_err_t api_forget_post(httpd_req_t *req)
{
    send_json(req, "200 OK", "{\"status\":\"erasing\"}\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    return wifi_portal_forget();            // Does not return.
}

static esp_err_t start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    // Default is 8. The portal uses 6 and hid_http adds 3 more; too few slots
    // silently fails registration with ESP_ERR_HTTPD_HANDLERS_FULL.
    cfg.max_uri_handlers = 16;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return ESP_FAIL;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t join = { .uri = "/join", .method = HTTP_POST, .handler = join_post };
    httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get };
    httpd_uri_t forget = { .uri = "/forget", .method = HTTP_GET, .handler = forget_get };
    httpd_uri_t forget_do = { .uri = "/forget", .method = HTTP_POST, .handler = forget_post };

    httpd_uri_t api_wifi = { .uri = "/api/wifi", .method = HTTP_POST, .handler = api_wifi_post };
    httpd_uri_t api_scan = { .uri = "/api/scan", .method = HTTP_GET, .handler = api_scan_get };
    httpd_uri_t api_status = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_get };
    httpd_uri_t api_forget = { .uri = "/api/forget", .method = HTTP_POST, .handler = api_forget_post };

    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &join);
    httpd_register_uri_handler(s_server, &scan);
    httpd_register_uri_handler(s_server, &forget);
    httpd_register_uri_handler(s_server, &forget_do);
    httpd_register_uri_handler(s_server, &api_wifi);
    httpd_register_uri_handler(s_server, &api_scan);
    httpd_register_uri_handler(s_server, &api_status);
    httpd_register_uri_handler(s_server, &api_forget);
    return ESP_OK;
}

// ---- startup ---------------------------------------------------------------

esp_err_t wifi_portal_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t ap = {
        .ap = {
            .ssid = WIFI_PORTAL_AP_SSID,
            .ssid_len = strlen(WIFI_PORTAL_AP_SSID),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    char ssid[SSID_MAX_LEN + 1] = { 0 };
    char pass[PASS_MAX_LEN + 1] = { 0 };
    // Holding BOOT at power-up skips the saved network, so a device that can no
    // longer reach its router still comes up with a usable portal.
    bool force_setup = reset_button_held_at_boot();
    if (force_setup) {
        ESP_LOGW(TAG, "setup forced by BOOT button; ignoring saved network");
    }

    if (!force_setup && load_credentials(ssid, pass, sizeof(ssid), sizeof(pass))) {
        wifi_config_t sta = { 0 };
        strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        ESP_LOGI(TAG, "connecting to saved network \"%s\"", ssid);
        esp_wifi_connect();
    }

    s_scan_done = false;
    esp_wifi_scan_start(NULL, false);

    if (start_http() != ESP_OK) {
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG, "portal up: join \"%s\", then open http://192.168.4.1/",
             WIFI_PORTAL_AP_SSID);
    return ESP_OK;
}

bool wifi_portal_scan_ready(void)
{
    return s_scan_done;
}

const char *wifi_portal_sta_ssid(void)
{
    return s_sta_ssid;
}

const char *wifi_portal_sta_ip(void)
{
    return s_sta_ip;
}

bool wifi_portal_sta_connected(void)
{
    return s_sta_connected;
}

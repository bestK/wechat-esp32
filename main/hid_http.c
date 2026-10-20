/*
 * HTTP endpoints for driving the HID keyboard, mouse and media keys.
 */

#include "hid_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_hidd_prf_api.h"
#include "hid_keyboard.h"
#include "wifi_portal.h"

static const char *TAG = "hid_http";

#define MAX_TEXT 64

// Percent-decoding for query values ("+" is a space in query strings too).
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

static bool query_field(const char *query, const char *name, char *out, size_t out_len)
{
    if (!query) {
        return false;
    }
    char key[16];
    snprintf(key, sizeof(key), "%s=", name);
    const char *p = strstr(query, key);
    if (!p) {
        return false;
    }
    p += strlen(key);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char raw[MAX_TEXT * 3 + 1];
    if (len >= sizeof(raw)) {
        len = sizeof(raw) - 1;
    }
    memcpy(raw, p, len);
    raw[len] = '\0';
    url_decode(out, raw, out_len);
    return true;
}

static bool is_ascii(const char *s)
{
    for (; *s; ++s) {
        if ((unsigned char)*s >= 0x80) {
            return false;
        }
    }
    return true;
}

static esp_err_t type_handler(httpd_req_t *req, bool newline)
{
    char query[MAX_TEXT * 3 + 32];
    char text[MAX_TEXT * 3 + 1];
    char decoded[MAX_TEXT + 1];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        !query_field(query, "text", text, sizeof(text))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing text\n");
    }
    strlcpy(decoded, text, sizeof(decoded));

    if (!is_ascii(decoded)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            "non-ASCII input: HID keycodes cannot type it\n");
    }
    if (!hid_keyboard_connected()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "HID not connected\n");
    }

    int sent = newline ? hid_keyboard_send_line(decoded)
                       : hid_keyboard_type(decoded);
    ESP_LOGI(TAG, "%s \"%s\" -> %d keystrokes",
             newline ? "line" : "type", decoded, sent);

    char reply[48];
    snprintf(reply, sizeof(reply), "%d\n", sent);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, reply);
}

static esp_err_t type_get(httpd_req_t *req)
{
    return type_handler(req, false);
}

static esp_err_t line_get(httpd_req_t *req)
{
    return type_handler(req, true);
}

// ---- Mouse and media keys --------------------------------------------------

// Bound on any single move so a typo cannot pin the HTTP task for minutes.
#define MAX_MOVE    4000
#define MAX_SCROLL  50

static bool query_int(const char *query, const char *name, int *out)
{
    char buf[16];
    if (!query_field(query, name, buf, sizeof(buf))) {
        return false;
    }
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf || *end) {
        return false;
    }
    *out = (int)v;
    return true;
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *body)
{
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_count(httpd_req_t *req, int n)
{
    char reply[16];
    snprintf(reply, sizeof(reply), "%d\n", n);
    return send_text(req, NULL, reply);
}

// Read the query string and check the HID link; sends the error reply itself.
static bool begin_request(httpd_req_t *req, char *query, size_t len)
{
    query[0] = '\0';
    httpd_req_get_url_query_str(req, query, len);   // Absent query is fine.
    if (!hid_keyboard_connected()) {
        send_text(req, "409 Conflict", "HID not connected\n");
        return false;
    }
    return true;
}

static bool in_range(int v, int limit)
{
    return v >= -limit && v <= limit;
}

static esp_err_t mouse_get(httpd_req_t *req)
{
    char query[96];
    if (!begin_request(req, query, sizeof(query))) {
        return ESP_OK;
    }
    int dx = 0, dy = 0;
    query_int(query, "dx", &dx);
    query_int(query, "dy", &dy);
    if (!in_range(dx, MAX_MOVE) || !in_range(dy, MAX_MOVE)) {
        return send_text(req, "400 Bad Request", "dx/dy out of range\n");
    }

    uint8_t buttons = 0;
    char click[8];
    if (query_field(query, "click", click, sizeof(click))) {
        if (!strcmp(click, "left")) buttons = HID_BTN_LEFT;
        else if (!strcmp(click, "right")) buttons = HID_BTN_RIGHT;
        else if (!strcmp(click, "middle")) buttons = HID_BTN_MIDDLE;
        else return send_text(req, "400 Bad Request", "click: left|right|middle\n");
    }

    int n = hid_mouse_move(dx, dy) + hid_mouse_click(buttons);
    ESP_LOGI(TAG, "mouse dx=%d dy=%d click=0x%x -> %d reports", dx, dy, buttons, n);
    return send_count(req, n);
}

static esp_err_t swipe_get(httpd_req_t *req)
{
    char query[96];
    if (!begin_request(req, query, sizeof(query))) {
        return ESP_OK;
    }
    int dx = 0, dy = 0;
    char dir[8];
    if (query_field(query, "dir", dir, sizeof(dir))) {
        int dist = 300;
        query_int(query, "dist", &dist);
        if (dist <= 0 || dist > MAX_MOVE) {
            return send_text(req, "400 Bad Request", "dist out of range\n");
        }
        if (!strcmp(dir, "up")) dy = -dist;
        else if (!strcmp(dir, "down")) dy = dist;
        else if (!strcmp(dir, "left")) dx = -dist;
        else if (!strcmp(dir, "right")) dx = dist;
        else return send_text(req, "400 Bad Request", "dir: up|down|left|right\n");
    } else {
        query_int(query, "dx", &dx);
        query_int(query, "dy", &dy);
    }
    if ((!dx && !dy) || !in_range(dx, MAX_MOVE) || !in_range(dy, MAX_MOVE)) {
        return send_text(req, "400 Bad Request", "need dir[&dist] or dx/dy\n");
    }

    int n = hid_mouse_swipe(dx, dy);
    ESP_LOGI(TAG, "swipe dx=%d dy=%d -> %d reports", dx, dy, n);
    return send_count(req, n);
}

static esp_err_t scroll_get(httpd_req_t *req)
{
    char query[64];
    if (!begin_request(req, query, sizeof(query))) {
        return ESP_OK;
    }
    int clicks = 0;
    if (!query_int(query, "n", &clicks) || !clicks || !in_range(clicks, MAX_SCROLL)) {
        return send_text(req, "400 Bad Request", "n: non-zero, -50..50\n");
    }
    int n = hid_mouse_scroll(clicks);
    ESP_LOGI(TAG, "scroll %d -> %d reports", clicks, n);
    return send_count(req, n);
}

static const struct {
    const char *name;
    uint16_t bits;
} k_media_keys[] = {
    { "home",       HID_CC_AC_HOME },
    { "search",     HID_CC_AC_SEARCH },
    { "back",       HID_CC_AC_BACK },
    { "keyboard",   HID_CC_KEYBOARD_TOGGLE },
    { "volup",      HID_CC_VOLUME_UP },
    { "voldown",    HID_CC_VOLUME_DOWN },
    { "mute",       HID_CC_MUTE },
    { "play",       HID_CC_PLAY_PAUSE },
    { "next",       HID_CC_NEXT_TRACK },
    { "prev",       HID_CC_PREV_TRACK },
    { "brightup",   HID_CC_BRIGHTNESS_UP },
    { "brightdown", HID_CC_BRIGHTNESS_DOWN },
};

static esp_err_t key_get(httpd_req_t *req)
{
    char query[64];
    if (!begin_request(req, query, sizeof(query))) {
        return ESP_OK;
    }
    char name[16];
    if (query_field(query, "name", name, sizeof(name))) {
        for (size_t i = 0; i < sizeof(k_media_keys) / sizeof(k_media_keys[0]); ++i) {
            if (!strcmp(name, k_media_keys[i].name)) {
                int n = hid_media_key(k_media_keys[i].bits);
                ESP_LOGI(TAG, "key %s -> %d", name, n);
                return send_count(req, n);
            }
        }
    }
    return send_text(req, "400 Bad Request",
        "name: home|search|back|keyboard|volup|voldown|mute|play|next|prev|brightup|brightdown\n");
}

static esp_err_t status_get(httpd_req_t *req)
{
    char reply[64];
    snprintf(reply, sizeof(reply), "{\"connected\":%s}\n",
             hid_keyboard_connected() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, reply);
}

esp_err_t hid_http_start(void)
{
    // Register on the portal's server rather than starting a second one: each
    // httpd instance reserves several sockets and CONFIG_LWIP_MAX_SOCKETS is 10.
    httpd_handle_t server = wifi_portal_http_server();
    if (!server) {
        ESP_LOGE(TAG, "portal HTTP server not running");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_uri_t type = { .uri = "/hid/type", .method = HTTP_GET, .handler = type_get };
    httpd_uri_t line = { .uri = "/hid/line", .method = HTTP_GET, .handler = line_get };
    httpd_uri_t status = { .uri = "/hid/status", .method = HTTP_GET, .handler = status_get };
    httpd_uri_t mouse = { .uri = "/hid/mouse", .method = HTTP_GET, .handler = mouse_get };
    httpd_uri_t swipe = { .uri = "/hid/swipe", .method = HTTP_GET, .handler = swipe_get };
    httpd_uri_t scroll = { .uri = "/hid/scroll", .method = HTTP_GET, .handler = scroll_get };
    httpd_uri_t key = { .uri = "/hid/key", .method = HTTP_GET, .handler = key_get };

    const httpd_uri_t *uris[] = { &type, &line, &status, &mouse, &swipe, &scroll, &key };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "registering %s failed: %s", uris[i]->uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "HID control at /hid/{type,line,status,mouse,swipe,scroll,key}");
    return ESP_OK;
}

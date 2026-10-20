/*
 * HTTP endpoints for driving the HID keyboard.
 *
 * Registered on a second httpd instance on port 81 so it stays independent of
 * the provisioning portal's page handlers.
 */

#include "hid_http.h"

#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
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

    esp_err_t err = httpd_register_uri_handler(server, &type);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &line);
    if (err == ESP_OK) err = httpd_register_uri_handler(server, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "registering HID endpoints failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "HID control at /hid/{type,line,status}");
    return ESP_OK;
}

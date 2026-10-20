/*
 * Minimal ANCS (Apple Notification Center Service) client for the ESP32-S3.
 *
 * The board advertises the ANCS service UUID as a 128-bit service solicitation.
 * A bonded iPhone sees that and connects to us; we then act as a GATT client on
 * the same link, subscribe to Notification Source + Data Source, and ask the
 * Control Point for the details of every newly added notification.
 *
 * Flow:
 *   CONNECT  -> create the GATTC virtual connection over the existing link
 *   OPEN     -> start encryption, negotiate MTU
 *   AUTH     -> bonded; discover the ANCS service
 *   SEARCH   -> find the three characteristics, enable notifications
 *   NOTIFY   -> Notification Source gives a UID, Data Source gives the text
 */

#include "ancs_client.h"

#include <string.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "ancs";

#define ANCS_APP_ID 0x56

// ANCS UUIDs, little-endian (the byte order the BLE stack wants).
static const uint8_t ANCS_SERVICE_UUID[16] = {
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79,
};
static const uint8_t ANCS_NOTIF_SOURCE_UUID[16] = {
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C,
    0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F,
};
static const uint8_t ANCS_CONTROL_POINT_UUID[16] = {
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98,
    0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69,
};
static const uint8_t ANCS_DATA_SOURCE_UUID[16] = {
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE,
    0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22,
};

// ANCS command / attribute ids.
#define ANCS_CMD_GET_ATTRS 0x00
#define ANCS_ATTR_APP_ID   0x00
#define ANCS_ATTR_TITLE    0x01
#define ANCS_ATTR_MESSAGE  0x03
#define ANCS_ATTR_COUNT    3

#define ANCS_EVT_ADDED     0

// Byte budgets. Chinese is 3 bytes per character in UTF-8, so these hold
// roughly 21 and 85 characters respectively.
#define ANCS_TITLE_MAX     64
#define ANCS_MESSAGE_MAX   256
#define ANCS_REASM_MAX     1024

#define ADV_TYPE_FLAGS         0x01
#define ADV_TYPE_NAME          0x09
#define ADV_TYPE_SOL_UUID128   0x15
#define ADV_FLAGS              0x06

static ancs_notify_cb_t s_cb;
static void *s_cb_ctx;

static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static bool s_connected;
static bool s_ready;
static bool s_mtu_done;
static bool s_search_started;

static esp_bd_addr_t s_peer_bda;
static esp_ble_addr_type_t s_peer_addr_type;
static uint16_t s_conn_id;
static bool s_have_conn;      // conn_id 0 is valid, so this cannot be inferred.

static uint16_t s_svc_start;
static uint16_t s_svc_end;
static uint16_t s_notif_source_handle;
static uint16_t s_control_point_handle;
static uint16_t s_data_source_handle;

// One GetNotificationAttributes response may span several Data Source packets.
static uint8_t s_reasm[ANCS_REASM_MAX];
static size_t s_reasm_len;

// iPhone can push several notifications back to back, so one pending slot is
// not enough: remember the category per UID until its attributes come back.
#define ANCS_PENDING_MAX 8
typedef struct {
    uint32_t uid;
    uint8_t category;
    bool used;
} ancs_pending_t;
static ancs_pending_t s_pending[ANCS_PENDING_MAX];

static void remember_category(uint32_t uid, uint8_t category)
{
    for (int i = 0; i < ANCS_PENDING_MAX; ++i) {
        if (s_pending[i].used && s_pending[i].uid == uid) {
            s_pending[i].category = category;
            return;
        }
    }
    for (int i = 0; i < ANCS_PENDING_MAX; ++i) {
        if (!s_pending[i].used) {
            s_pending[i].used = true;
            s_pending[i].uid = uid;
            s_pending[i].category = category;
            return;
        }
    }
    // Table full: reuse slot 0. Only the category label is lost.
    s_pending[0].used = true;
    s_pending[0].uid = uid;
    s_pending[0].category = category;
}

static uint8_t take_category(uint32_t uid, bool *found)
{
    for (int i = 0; i < ANCS_PENDING_MAX; ++i) {
        if (s_pending[i].used && s_pending[i].uid == uid) {
            uint8_t category = s_pending[i].category;
            s_pending[i].used = false;
            *found = true;
            return category;
        }
    }
    *found = false;
    return 0;
}

// ---- Duplicate suppression ------------------------------------------------
//
// iOS replays every notification still sitting in Notification Centre each time
// we reconnect, so the same UID arrives again after every reboot. Remember the
// last few notifications in NVS and skip repeats.
//
// UID alone is not enough: iOS may reuse a UID for a different message once the
// original is dismissed. Pairing the UID with a hash of the text keeps a reused
// UID with new content from being swallowed.

#define DEDUP_NS        "ancs"
#define DEDUP_KEY       "seen"
#define DEDUP_MAX       16

typedef struct {
    uint32_t uid;
    uint32_t text_hash;   // FNV-1a over app_id + title + message.
    bool valid;
} dedup_entry_t;

typedef struct {
    uint32_t magic;       // Bumped when the layout changes.
    dedup_entry_t entries[DEDUP_MAX];
} dedup_store_t;

#define DEDUP_MAGIC 0x414E4353u   // "ANCS"

static dedup_store_t s_seen;
static bool s_seen_loaded;
static size_t s_seen_next;        // Round-robin insert position.

static uint32_t notification_hash(const ancs_notification_t *n)
{
    // Hash the three fields that identify a message, separated so that
    // "ab"+"c" and "a"+"bc" do not collide.
    uint32_t h = 2166136261u;
    const char *parts[] = { n->app_id, n->title, n->message };
    for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); ++p) {
        for (const char *c = parts[p]; *c; ++c) {
            h ^= (uint8_t)*c;
            h *= 16777619u;
        }
        h ^= 0xFFu;           // Field separator.
        h *= 16777619u;
    }
    return h;
}

static void dedup_load(void)
{
    if (s_seen_loaded) {
        return;
    }
    s_seen_loaded = true;

    nvs_handle_t nvs;
    if (nvs_open(DEDUP_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;                       // Nothing stored yet.
    }
    size_t len = sizeof(s_seen);
    esp_err_t err = nvs_get_blob(nvs, DEDUP_KEY, &s_seen, &len);
    nvs_close(nvs);
    if (err != ESP_OK || len != sizeof(s_seen) || s_seen.magic != DEDUP_MAGIC) {
        memset(&s_seen, 0, sizeof(s_seen));
        s_seen.magic = DEDUP_MAGIC;
    }
}

static void dedup_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(DEDUP_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_blob(nvs, DEDUP_KEY, &s_seen, sizeof(s_seen));
    nvs_commit(nvs);
    nvs_close(nvs);
}

// Returns true when this notification was already delivered before.
static bool dedup_seen(uint32_t uid, uint32_t text_hash)
{
    dedup_load();
    for (int i = 0; i < DEDUP_MAX; ++i) {
        if (s_seen.entries[i].valid &&
            s_seen.entries[i].uid == uid &&
            s_seen.entries[i].text_hash == text_hash) {
            return true;
        }
    }
    return false;
}

static void dedup_remember(uint32_t uid, uint32_t text_hash)
{
    dedup_load();
    dedup_entry_t *slot = &s_seen.entries[s_seen_next];
    slot->uid = uid;
    slot->text_hash = text_hash;
    slot->valid = true;
    s_seen_next = (s_seen_next + 1) % DEDUP_MAX;
    dedup_save();
}

static char s_name[32] = "WeChat-Watch";
static uint8_t s_adv[31];
static uint8_t s_adv_len;
static uint8_t s_scan_rsp[31];
static uint8_t s_scan_rsp_len;

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x40,
    .adv_int_max = 0x80,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// ---- helpers ---------------------------------------------------------------

static void copy_truncated(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t n = src_len < dst_len - 1 ? src_len : dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint16_t find_cccd_handle(uint16_t char_handle)
{
    esp_gattc_descr_elem_t descr;
    uint16_t count = 1;
    esp_bt_uuid_t cccd_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG },
    };
    esp_gatt_status_t status = esp_ble_gattc_get_descr_by_char_handle(
        s_gattc_if, s_conn_id, char_handle, cccd_uuid, &descr, &count);
    if (status != ESP_GATT_OK || count == 0) {
        return 0;
    }
    return descr.handle;
}

static uint16_t find_char_handle(const uint8_t *uuid128)
{
    esp_gattc_char_elem_t elem;
    uint16_t count = 1;
    esp_bt_uuid_t uuid = { .len = ESP_UUID_LEN_128 };
    memcpy(uuid.uuid.uuid128, uuid128, 16);

    uint16_t start = s_svc_start ? s_svc_start : 0x0001;
    uint16_t end = s_svc_end ? s_svc_end : 0xFFFF;

    esp_gatt_status_t status = esp_ble_gattc_get_char_by_uuid(
        s_gattc_if, s_conn_id, start, end, uuid, &elem, &count);
    if (status != ESP_GATT_OK || count == 0) {
        return 0;
    }
    return elem.char_handle;
}

static void subscribe_notify(uint16_t char_handle)
{
    esp_ble_gattc_register_for_notify(s_gattc_if, s_peer_bda, char_handle);

    uint16_t cccd = find_cccd_handle(char_handle);
    if (!cccd) {
        ESP_LOGW(TAG, "no CCCD for char %u", char_handle);
        return;
    }
    uint8_t enable[2] = { 0x01, 0x00 };
    esp_err_t err = esp_ble_gattc_write_char_descr(s_gattc_if, s_conn_id, cccd,
                                                   sizeof(enable), enable,
                                                   ESP_GATT_WRITE_TYPE_RSP,
                                                   ESP_GATT_AUTH_REQ_NO_MITM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cccd write failed: %s", esp_err_to_name(err));
    }
}

// ---- Control Point: ask for the text of one notification -------------------

static void request_attributes(uint32_t uid)
{
    if (!s_control_point_handle) {
        return;
    }

    uint8_t cmd[12];
    size_t off = 0;
    cmd[off++] = ANCS_CMD_GET_ATTRS;
    cmd[off++] = uid & 0xff;
    cmd[off++] = (uid >> 8) & 0xff;
    cmd[off++] = (uid >> 16) & 0xff;
    cmd[off++] = (uid >> 24) & 0xff;
    cmd[off++] = ANCS_ATTR_APP_ID;                       // no length field
    cmd[off++] = ANCS_ATTR_TITLE;
    cmd[off++] = ANCS_TITLE_MAX & 0xff;
    cmd[off++] = (ANCS_TITLE_MAX >> 8) & 0xff;
    cmd[off++] = ANCS_ATTR_MESSAGE;
    cmd[off++] = ANCS_MESSAGE_MAX & 0xff;
    cmd[off++] = (ANCS_MESSAGE_MAX >> 8) & 0xff;

    s_reasm_len = 0;
    esp_err_t err = esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_control_point_handle,
                                             off, cmd, ESP_GATT_WRITE_TYPE_RSP,
                                             ESP_GATT_AUTH_REQ_NO_MITM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "control point write failed: %s", esp_err_to_name(err));
    }
}

// ---- Notification Source ---------------------------------------------------

static void on_notification_source(const uint8_t *data, uint16_t len)
{
    if (len < 8) {
        return;
    }
    uint8_t event_id = data[0];
    uint8_t category = data[2];
    uint32_t uid = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                   ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

    if (event_id != ANCS_EVT_ADDED) {
        return;                                          // Ignore modified/removed.
    }
    ESP_LOGI(TAG, "added: uid=%lu category=%u", (unsigned long)uid, category);
    // Category only exists in this event, but the notification is emitted from
    // the Data Source reply, so remember it until the attributes arrive.
    remember_category(uid, category);
    request_attributes(uid);
}

// ---- Data Source: reassemble and parse the attribute tuples ----------------

static void on_data_source(const uint8_t *data, uint16_t len)
{
    if (s_reasm_len + len > sizeof(s_reasm)) {
        ESP_LOGW(TAG, "data source overflow, dropping");
        s_reasm_len = 0;
        return;
    }
    memcpy(s_reasm + s_reasm_len, data, len);
    s_reasm_len += len;

    // Layout: cmd(1) uid(4) then [attr_id(1)][len(2 LE)][value] repeated.
    if (s_reasm_len < 5 || s_reasm[0] != ANCS_CMD_GET_ATTRS) {
        return;
    }
    uint32_t uid = (uint32_t)s_reasm[1] | ((uint32_t)s_reasm[2] << 8) |
                   ((uint32_t)s_reasm[3] << 16) | ((uint32_t)s_reasm[4] << 24);

    char app_id[64] = { 0 };
    char title[ANCS_TITLE_MAX + 1] = { 0 };
    char message[ANCS_MESSAGE_MAX + 1] = { 0 };
    bool got_app = false;

    size_t off = 5;
    int parsed = 0;
    while (parsed < ANCS_ATTR_COUNT) {
        if (off + 3 > s_reasm_len) {
            return;                                      // Header not here yet.
        }
        uint8_t attr_id = s_reasm[off];
        uint16_t attr_len = s_reasm[off + 1] | ((uint16_t)s_reasm[off + 2] << 8);
        off += 3;
        if (off + attr_len > s_reasm_len) {
            return;                                      // Tuple still in flight.
        }

        const char *val = (const char *)(s_reasm + off);
        switch (attr_id) {
        case ANCS_ATTR_APP_ID:
            copy_truncated(app_id, sizeof(app_id), val, attr_len);
            got_app = true;
            break;
        case ANCS_ATTR_TITLE:
            copy_truncated(title, sizeof(title), val, attr_len);
            break;
        case ANCS_ATTR_MESSAGE:
            copy_truncated(message, sizeof(message), val, attr_len);
            break;
        default:
            break;
        }
        off += attr_len;
        parsed++;
    }

    s_reasm_len = 0;
    if (!got_app) {
        return;
    }

    bool have_category = false;
    uint8_t category = take_category(uid, &have_category);

    ancs_notification_t n = {
        .uid = uid,
        .category = have_category ? category : 0,
        .app_id = app_id,
        .title = title,
        .message = message,
    };

    uint32_t hash = notification_hash(&n);
    if (dedup_seen(uid, hash)) {
        ESP_LOGD(TAG, "skipping replayed notification uid=%lu", (unsigned long)uid);
        return;
    }
    dedup_remember(uid, hash);

    if (s_cb) {
        s_cb(&n, s_cb_ctx);
    }
}

// ---- discovery -------------------------------------------------------------

static void discover_ancs(void)
{
    s_notif_source_handle = find_char_handle(ANCS_NOTIF_SOURCE_UUID);
    s_control_point_handle = find_char_handle(ANCS_CONTROL_POINT_UUID);
    s_data_source_handle = find_char_handle(ANCS_DATA_SOURCE_UUID);

    ESP_LOGI(TAG, "chars: notif=%u control=%u data=%u",
             s_notif_source_handle, s_control_point_handle, s_data_source_handle);

    if (!s_notif_source_handle || !s_control_point_handle || !s_data_source_handle) {
        ESP_LOGW(TAG, "ANCS service incomplete on peer; is the iPhone bonded?");
        return;
    }

    // Data Source first, so an attribute reply can never arrive unsubscribed.
    subscribe_notify(s_data_source_handle);
    subscribe_notify(s_notif_source_handle);
    s_ready = true;
    ESP_LOGI(TAG, "ready, waiting for notifications");
}

static void start_search(void)
{
    // conn_id 0 is a valid Bluedroid connection id, so "connected" has to be
    // tracked separately from the id itself.
    if (s_search_started || !s_have_conn) {
        return;
    }
    s_search_started = true;

    esp_bt_uuid_t uuid = { .len = ESP_UUID_LEN_128 };
    memcpy(uuid.uuid.uuid128, ANCS_SERVICE_UUID, 16);
    esp_err_t err = esp_ble_gattc_search_service(s_gattc_if, s_conn_id, &uuid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "search_service failed: %s", esp_err_to_name(err));
        s_search_started = false;
    } else {
        ESP_LOGI(TAG, "searching ANCS service on conn=%u", s_conn_id);
    }
}

// ---- GATTC events ----------------------------------------------------------

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                     esp_ble_gattc_cb_param_t *param)
{
    ESP_LOGD(TAG, "gattc event %d", event);
    switch (event) {
    case ESP_GATTC_REG_EVT:
        s_gattc_if = gattc_if;
        ESP_LOGI(TAG, "gattc registered, if=%u", gattc_if);
        break;

    case ESP_GATTC_CONNECT_EVT:
        // Physical link is up (the iPhone connected to our advertisement).
        // Hold it from the GATT client side so we can use it as a client.
        memcpy(s_peer_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        s_peer_addr_type = param->connect.ble_addr_type;
        s_connected = true;
        ESP_LOGI(TAG, "link up, addr_type=%d", s_peer_addr_type);

        esp_ble_conn_update_params_t conn = { 0 };
        memcpy(conn.bda, s_peer_bda, sizeof(esp_bd_addr_t));
        conn.min_int = 0x10;
        conn.max_int = 0x30;
        conn.latency = 0;
        conn.timeout = 400;
        esp_ble_gap_update_conn_params(&conn);

        esp_ble_gattc_open(gattc_if, s_peer_bda, s_peer_addr_type, true);
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK && param->open.status != ESP_GATT_ALREADY_OPEN) {
            ESP_LOGW(TAG, "gattc open failed: 0x%x", param->open.status);
            break;
        }
        s_conn_id = param->open.conn_id;
        s_have_conn = true;
        ESP_LOGI(TAG, "gattc open, conn=%u", s_conn_id);

        // iPhone only exposes ANCS over an encrypted, bonded link.
        esp_ble_set_encryption(s_peer_bda, ESP_BLE_SEC_ENCRYPT);
        esp_ble_gattc_send_mtu_req(gattc_if, s_conn_id);
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        ESP_LOGI(TAG, "mtu=%u", param->cfg_mtu.mtu);
        s_mtu_done = true;
        start_search();
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_128 &&
            memcmp(param->search_res.srvc_id.uuid.uuid.uuid128, ANCS_SERVICE_UUID, 16) == 0) {
            s_svc_start = param->search_res.start_handle;
            s_svc_end = param->search_res.end_handle;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "service search failed: 0x%x", param->search_cmpl.status);
            s_search_started = false;
            break;
        }
        if (!s_svc_start) {
            // Log what the peer *does* expose: if ANCS is absent the iPhone
            // has not granted notification access to this accessory.
            esp_gattc_service_elem_t svc;
            uint16_t count = 1;
            esp_gatt_status_t st = esp_ble_gattc_get_service(gattc_if, s_conn_id, NULL, &svc, &count, 0);
            ESP_LOGW(TAG, "ANCS service not found (status=%d, first svc uuid len=%u)",
                     st, count ? svc.uuid.len : 0);
            if (st == ESP_GATT_OK && count) {
                if (svc.uuid.len == ESP_UUID_LEN_16) {
                    ESP_LOGW(TAG, "  first service: 0x%04x [%u..%u]", svc.uuid.uuid.uuid16,
                             svc.start_handle, svc.end_handle);
                } else if (svc.uuid.len == ESP_UUID_LEN_128) {
                    ESP_LOGW(TAG, "  first service: %02x%02x%02x%02x-...",
                             svc.uuid.uuid.uuid128[15], svc.uuid.uuid.uuid128[14],
                             svc.uuid.uuid.uuid128[13], svc.uuid.uuid.uuid128[12]);
                }
            }
            break;
        }
        discover_ancs();
        break;

    case ESP_GATTC_NOTIFY_EVT:
        if (param->notify.handle == s_notif_source_handle) {
            on_notification_source(param->notify.value, param->notify.value_len);
        } else if (param->notify.handle == s_data_source_handle) {
            on_data_source(param->notify.value, param->notify.value_len);
        }
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "char write failed: 0x%x", param->write.status);
        }
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "descriptor write failed: 0x%x", param->write.status);
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
        ESP_LOGI(TAG, "disconnected: reason=0x%x", param->disconnect.reason);
        s_connected = false;
        s_ready = false;
        s_mtu_done = false;
        s_search_started = false;
        s_conn_id = 0;
        s_have_conn = false;
        s_svc_start = s_svc_end = 0;
        s_notif_source_handle = s_control_point_handle = s_data_source_handle = 0;
        s_reasm_len = 0;
        memset(s_pending, 0, sizeof(s_pending));
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    default:
        break;
    }
}

// ---- GAP events ------------------------------------------------------------

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "advertising as \"%s\"", s_name);
        } else {
            ESP_LOGE(TAG, "advertising failed: 0x%x", param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        // The iPhone is asking to pair: accept and start bonding.
        ESP_LOGI(TAG, "security request from peer, accepting");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
        bool ok = param->ble_security.auth_cmpl.success;
        ESP_LOGI(TAG, "auth %s (mode=0x%x, key_present=%d)",
                 ok ? "ok" : "failed",
                 param->ble_security.auth_cmpl.auth_mode,
                 param->ble_security.auth_cmpl.key_present);
        if (!ok) {
            ESP_LOGW(TAG, "bonding failed; remove the device on the iPhone and retry");
            break;
        }
        // Encryption may finish before or after the MTU exchange; search once
        // both have happened.
        if (s_mtu_done) {
            start_search();
        }
        break;
    }

    case ESP_GAP_BLE_NC_REQ_EVT:
        esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
        break;

    default:
        break;
    }
}

// ---- advertising payload ---------------------------------------------------

static void build_adv(void)
{
    size_t off = 0;
    s_adv[off++] = 2;
    s_adv[off++] = ADV_TYPE_FLAGS;
    s_adv[off++] = ADV_FLAGS;

    // 128-bit service solicitation for ANCS: this is what makes the iPhone
    // connect to us instead of us scanning for it.
    s_adv[off++] = 1 + 16;
    s_adv[off++] = ADV_TYPE_SOL_UUID128;
    memcpy(s_adv + off, ANCS_SERVICE_UUID, 16);
    off += 16;
    s_adv_len = off;

    size_t name_len = strlen(s_name);
    if (name_len > sizeof(s_scan_rsp) - 2) {
        name_len = sizeof(s_scan_rsp) - 2;
    }
    s_scan_rsp[0] = name_len + 1;
    s_scan_rsp[1] = ADV_TYPE_NAME;
    memcpy(s_scan_rsp + 2, s_name, name_len);
    s_scan_rsp_len = name_len + 2;
}

// ---- public API ------------------------------------------------------------

void ancs_client_set_name(const char *name)
{
    if (name && name[0]) {
        strlcpy(s_name, name, sizeof(s_name));
    }
}

esp_err_t ancs_client_init(ancs_notify_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;

    // Bonding with Secure Connections, no MITM: this board has no display or
    // keypad, so iOS falls back to Just Works and cannot verify a passkey.
    // Asking for MITM here would only log a downgrade warning.
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t io_cap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t key_mask = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_err_t err = esp_ble_gap_register_callback(gap_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gap callback register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ble_gattc_register_callback(gattc_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gattc callback register failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, sizeof(io_cap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &key_mask, sizeof(key_mask));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &key_mask, sizeof(key_mask));

    esp_ble_gap_set_device_name(s_name);
    esp_ble_gap_config_local_icon(ESP_BLE_APPEARANCE_GENERIC_WATCH);

    err = esp_ble_gatt_set_local_mtu(517);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set local mtu failed: %s", esp_err_to_name(err));
    }

    err = esp_ble_gattc_app_register(ANCS_APP_ID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gattc app register failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ANCS client initialised");
    return ESP_OK;
}

esp_err_t ancs_client_start_advertising(void)
{
    if (s_gattc_if == ESP_GATT_IF_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    build_adv();

    esp_err_t err = esp_ble_gap_config_scan_rsp_data_raw(s_scan_rsp, s_scan_rsp_len);
    if (err != ESP_OK) {
        return err;
    }
    return esp_ble_gap_config_adv_data_raw(s_adv, s_adv_len);
}

bool ancs_client_connected(void)
{
    return s_connected;
}

bool ancs_client_ready(void)
{
    return s_ready;
}

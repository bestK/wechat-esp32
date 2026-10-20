idf_component_register(
    SRCS
        "app_main.c"
        "ancs_client.c"
        "hid_keyboard.c"
        "hid_http.c"
        "wifi_portal.c"
        "reset_button.c"
        "hid/hid_device_le_prf.c"
        "hid/hid_dev.c"
        "hid/esp_hidd_prf_api.c"
    INCLUDE_DIRS
        "."
        "hid"
    REQUIRES
        bt
        nvs_flash
        esp_timer
        esp_http_server
        esp_wifi
        esp_netif
        esp_event
        driver
)

# The HID profile is copied verbatim from the ESP-IDF ble_hid_device_demo
# example, which ships a few unused property constants. Keep the source
# untouched so it stays diffable against upstream.
set_source_files_properties(
    "hid/hid_device_le_prf.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-const-variable"
)

idf_component_register(
    SRCS "main.c"
         "home_logic.c"
         "home_json.c"
         "home_store.c"
         "home_net.c"
         "home_console.c"
         "home_ui.c"
         "spot_json.c"
         "spot_logic.c"
         "spot_net.c"
         "setup_logic.c"
         "setup_portal.c"
         "demo_radio.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "setup_page.html"
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             esp_http_client esp-tls mbedtls lwip heap esp_driver_usb_serial_jtag vfs
             esp_http_server
)

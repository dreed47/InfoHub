set(INFOHUB_MAIN_REQUIRES
    app_update
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_pm
    esp_https_ota
    esp_driver_gpio
    esp_event
    esp_http_client
    esp_http_server
    mqtt
    esp_netif
    lwip
    espressif__esp_new_jpeg
    espressif__esp_codec_dev
    esp-tls
    esp_wifi
    libpng
    lvgl
    nvs_flash
    joltwallet__littlefs
)

set(INFOHUB_MAIN_PRIV_REQUIRES)

set(INFOHUB_PLUGIN_SRCS)
set(INFOHUB_PLUGIN_EMBED_TXTFILES)
if(CONFIG_INFOHUB_PLUGIN_PRINTER)
    list(APPEND INFOHUB_PLUGIN_SRCS
        "src/plugins/printer/bambu_status.cpp"
        "src/plugins/printer/bambu_cloud_client.cpp"
        "src/plugins/printer/error_lookup.cpp"
        "src/plugins/printer/printer_plugin.cpp"
        "src/plugins/printer/printer_plugin_portal.cpp"
        "src/plugins/printer/printer_plugin_ui.cpp"
        "src/plugins/printer/p1s_camera_client.cpp"
        "src/plugins/printer/printer_client.cpp"
        "src/plugins/printer/printer_state.cpp"
        "src/plugins/printer/status_resolver.cpp"
        # bambuicon (logo) / mdi_40 are printer-content-only as of Phase
        # 4b/4c — moved out of the unconditional font/image asset list below,
        # into here. dosis_32 stays unconditional below: weather and stocks
        # both use it too (confirmed via grep), a printer-disabled build
        # fails to link without it.
        "include/img/bambuicon.c"
        "include/font/mdi_40.c"
    )
    list(APPEND INFOHUB_PLUGIN_EMBED_TXTFILES
        "include/infohub/plugins/printer/bambu.cert"
        "include/infohub/plugins/printer/bambu_p2s_250626.cert"
        "include/infohub/plugins/printer/bambu_h2c_251122.cert"
        "include/infohub/plugins/printer/bambu_x2c_260425.cert"
        "include/infohub/plugins/printer/error_lookup.tsv"
    )
endif()
# mqtt (local MQTT status/commands) and espressif__esp_new_jpeg (P1S camera
# JPEG decode) are exclusively used by printer-plugin code, but stay in the
# unconditional REQUIRES list above rather than gated behind
# CONFIG_INFOHUB_PLUGIN_PRINTER: ESP-IDF evaluates each component's
# CMakeLists.txt twice — an early pass that builds the global component
# dependency graph (where CONFIG_* Kconfig variables are NOT yet populated,
# confirmed empirically), then a second real pass. A REQUIRES list gated on
# a CONFIG_* value is silently wrong from that first pass onward, breaking
# the build with "not in the requirements list" errors regardless of the
# option's actual value. SRCS/EMBED_TXTFILES gating (above and elsewhere in
# this file) doesn't have this problem — only actual file inclusion matters
# there. With printer's own .cpp files excluded via SRCS when the plugin is
# disabled, nothing calls into mqtt/jpeg, and the linker's --gc-sections
# strips their unused code from the final image regardless.

if(CONFIG_INFOHUB_PLUGIN_WEATHER)
    list(APPEND INFOHUB_PLUGIN_SRCS
        "src/plugins/weather/weather_flow_client.cpp"
        "src/plugins/weather/weather_plugin.cpp"
        "src/plugins/weather/weather_plugin_portal.cpp"
    )
endif()

if(CONFIG_INFOHUB_PLUGIN_STOCKS)
    list(APPEND INFOHUB_PLUGIN_SRCS
        "src/plugins/stocks/stock_client.cpp"
        "src/plugins/stocks/stocks_plugin.cpp"
        "src/plugins/stocks/stocks_plugin_portal.cpp"
    )
    list(APPEND INFOHUB_PLUGIN_EMBED_TXTFILES
        # Alpha Vantage's cert chain (alphavantage.co -> Google Trust
        # Services WE1 -> GTS Root R4) isn't covered by this project's
        # pruned "common" mbedTLS cert bundle (CONFIG_MBEDTLS_CERTIFICATE_
        # BUNDLE_DEFAULT_CMN in sdkconfig.defaults, not _FULL) -- pin the
        # root directly, same EMBED_TXTFILES + esp_http_client cert_pem
        # pattern as the printer plugin's Bambu certs. Fetched directly
        # from https://pki.goog/repo/certs/gtsr4.pem (self-signed root,
        # valid to 2036), not fabricated.
        "include/infohub/plugins/stocks/gts_root_r4.cert"
    )
endif()

if("${INFOHUB_HW_COMPILE_DEFINE}" STREQUAL "INFOHUB_HW_VARIANT_AMOLED_1_75=1")
    list(APPEND INFOHUB_MAIN_PRIV_REQUIRES
        XPowersLib
        esp32_s3_touch_amoled_1_75
    )
elseif("${INFOHUB_HW_COMPILE_DEFINE}" STREQUAL "INFOHUB_HW_VARIANT_LCD_2_8C=1")
    list(APPEND INFOHUB_MAIN_PRIV_REQUIRES
        esp_adc
        esp32_s3_touch_lcd_2_8c
    )
endif()

idf_component_register(
    SRCS
        "app_main.cpp"
        "src/application.cpp"
        "src/audio_notifier.cpp"
        "src/config_store.cpp"
        "src/network_arbiter.cpp"
        "src/plugin_registry.cpp"
        "include/font/dosis_20.c"
        "include/font/dosis_32.c"
        "include/font/dosis_40.c"
        "include/font/dosis_64.c"
        "include/font/mdi_30.c"
        "src/pmu.cpp"
        "src/debug_log_buffer.cpp"
        "src/setup_portal.cpp"
        "src/serial_provisioner.cpp"
        "src/time_sync.cpp"
        "src/ui.cpp"
        "src/ui_shell.cpp"
        "src/ui_toolkit.cpp"
        "src/wifi_manager.cpp"
        ${INFOHUB_PLUGIN_SRCS}
    INCLUDE_DIRS
        "include"
    EMBED_TXTFILES
        ${INFOHUB_PLUGIN_EMBED_TXTFILES}
    REQUIRES
        ${INFOHUB_MAIN_REQUIRES}
    PRIV_REQUIRES
        ${INFOHUB_MAIN_PRIV_REQUIRES}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    INFOHUB_RELEASE_VERSION="${INFOHUB_RELEASE_VERSION}"
    ${INFOHUB_HW_COMPILE_DEFINE}
)

if("${INFOHUB_HW_COMPILE_DEFINE}" STREQUAL "INFOHUB_HW_VARIANT_AMOLED_1_75=1")
    idf_component_get_property(INFOHUB_BSP_DIR esp32_s3_touch_amoled_1_75 COMPONENT_DIR)
    idf_component_get_property(INFOHUB_BSP_LIB esp32_s3_touch_amoled_1_75 COMPONENT_LIB)
    idf_component_get_property(INFOHUB_XPOWERS_DIR XPowersLib COMPONENT_DIR)
    idf_component_get_property(INFOHUB_XPOWERS_LIB XPowersLib COMPONENT_LIB)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${INFOHUB_BSP_DIR}/include"
        "${INFOHUB_XPOWERS_DIR}/src"
    )
    target_link_libraries(${COMPONENT_LIB} PRIVATE
        ${INFOHUB_BSP_LIB}
        ${INFOHUB_XPOWERS_LIB}
    )
elseif("${INFOHUB_HW_COMPILE_DEFINE}" STREQUAL "INFOHUB_HW_VARIANT_LCD_2_8C=1")
    idf_component_get_property(INFOHUB_BSP_DIR esp32_s3_touch_lcd_2_8c COMPONENT_DIR)
    idf_component_get_property(INFOHUB_BSP_LIB esp32_s3_touch_lcd_2_8c COMPONENT_LIB)
    idf_component_get_property(INFOHUB_ADC_DIR esp_adc COMPONENT_DIR)
    idf_component_get_property(INFOHUB_ADC_LIB esp_adc COMPONENT_LIB)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${INFOHUB_BSP_DIR}/include"
        "${INFOHUB_ADC_DIR}/include"
    )
    target_link_libraries(${COMPONENT_LIB} PRIVATE
        ${INFOHUB_BSP_LIB}
        ${INFOHUB_ADC_LIB}
    )
endif()

string(FIND "${INFOHUB_RELEASE_VERSION}" "-debug" _debug_pos)
string(FIND "${INFOHUB_RELEASE_VERSION}" "_debug" _debug_pos2)
if(_debug_pos GREATER -1 OR _debug_pos2 GREATER -1)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE INFOHUB_DEBUG_BUILD=1)
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)

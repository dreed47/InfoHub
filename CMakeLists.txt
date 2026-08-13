cmake_minimum_required(VERSION 3.24)

set(IDF_TARGET "esp32s3")
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(INFOHUB_RELEASE_VERSION "v0.1.0-alpha")
set(PROJECT_VER "${INFOHUB_RELEASE_VERSION}")
set(INFOHUB_HW_VARIANT "lcd_2_8c" CACHE STRING
    "InfoHub hardware variant: lcd_2_8c or amoled_1_75")
set_property(CACHE INFOHUB_HW_VARIANT PROPERTY STRINGS "lcd_2_8c" "amoled_1_75")

if(INFOHUB_HW_VARIANT STREQUAL "lcd_2_8c")
    set(INFOHUB_HW_VARIANT_LABEL "2.8C")
    set(INFOHUB_BSP_COMPONENT "esp32_s3_touch_lcd_2_8c")
    set(INFOHUB_HW_COMPILE_DEFINE "INFOHUB_HW_VARIANT_LCD_2_8C=1")
    set(INFOHUB_RELEASE_ROOT "${CMAKE_SOURCE_DIR}/release/2.8c")
    set(INFOHUB_RELEASE_VERSION_SUFFIX "${INFOHUB_RELEASE_VERSION}-2.8c")
    set(EXCLUDE_COMPONENTS esp32_s3_touch_amoled_1_75)
elseif(INFOHUB_HW_VARIANT STREQUAL "amoled_1_75")
    set(INFOHUB_HW_VARIANT_LABEL "1.75 AMOLED")
    set(INFOHUB_BSP_COMPONENT "esp32_s3_touch_amoled_1_75")
    set(INFOHUB_HW_COMPILE_DEFINE "INFOHUB_HW_VARIANT_AMOLED_1_75=1")
    set(INFOHUB_RELEASE_ROOT "${CMAKE_SOURCE_DIR}/release")
    set(INFOHUB_RELEASE_VERSION_SUFFIX "${INFOHUB_RELEASE_VERSION}")
    set(EXCLUDE_COMPONENTS esp32_s3_touch_lcd_2_8c)
else()
    message(FATAL_ERROR
        "Unsupported INFOHUB_HW_VARIANT='${INFOHUB_HW_VARIANT}'. "
        "Use 'lcd_2_8c' or 'amoled_1_75'.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(infohub_idf)

# Work around an Xtensa GCC ICE seen in ESP-IDF 5.5.2 while compiling
# esp_lcd_panel_rgb.c with the default debug/-Og flags on Windows.
idf_component_get_property(ESP_LCD_DIR esp_lcd COMPONENT_DIR)
idf_component_get_property(ESP_LCD_LIB esp_lcd COMPONENT_LIB)
if(ESP_LCD_DIR AND ESP_LCD_LIB)
    set_source_files_properties(
        "${ESP_LCD_DIR}/rgb/esp_lcd_panel_rgb.c"
        TARGET_DIRECTORY ${ESP_LCD_LIB}
        PROPERTIES
            COMPILE_OPTIONS "-O0"
    )
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(RELEASE_INITIAL_FLASH_DIR "${INFOHUB_RELEASE_ROOT}/initial")
set(RELEASE_INITIAL_FLASH_IMAGE "${RELEASE_INITIAL_FLASH_DIR}/infohub_full.bin")
set(RELEASE_INITIAL_FLASH_VERSIONED_IMAGE
    "${RELEASE_INITIAL_FLASH_DIR}/archive/infohub_full-${INFOHUB_RELEASE_VERSION_SUFFIX}.bin")
set(RELEASE_OTA_DIR "${INFOHUB_RELEASE_ROOT}/ota")
set(RELEASE_OTA_IMAGE "${RELEASE_OTA_DIR}/infohub_ota.bin")
set(RELEASE_OTA_VERSIONED_IMAGE
    "${RELEASE_OTA_DIR}/archive/infohub_ota-${INFOHUB_RELEASE_VERSION_SUFFIX}.bin")
set(RELEASE_INITIAL_FLASH_PACKAGER "${CMAKE_SOURCE_DIR}/tools/package_initial_flash.py")
# Error lookup TSV is embedded directly into the app binary via EMBED_TXTFILES
# in main/CMakeLists.txt — no SPIFFS partition needed.

add_custom_target(
    release_initial_flash
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_INITIAL_FLASH_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_OTA_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${RELEASE_INITIAL_FLASH_PACKAGER}"
            --build-dir "${CMAKE_BINARY_DIR}"
            --release-root "${INFOHUB_RELEASE_ROOT}"
            --output "${RELEASE_INITIAL_FLASH_IMAGE}"
            --ota-output "${RELEASE_OTA_IMAGE}"
            --version "${INFOHUB_RELEASE_VERSION_SUFFIX}"
    BYPRODUCTS
            "${RELEASE_INITIAL_FLASH_IMAGE}"
            "${RELEASE_INITIAL_FLASH_VERSIONED_IMAGE}"
            "${RELEASE_OTA_IMAGE}"
            "${RELEASE_OTA_VERSIONED_IMAGE}"
    DEPENDS
            app
            bootloader
            "${CMAKE_BINARY_DIR}/flasher_args.json"
            "${RELEASE_INITIAL_FLASH_PACKAGER}"
    COMMENT "Packaging ${INFOHUB_HW_VARIANT_LABEL} release images"
    VERBATIM
)

#include "bsp/esp32_s3_touch_lcd_2_8c.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_lv_adapter_input.h"
#include "esp_lcd_touch.h"
#include "esp_io_expander.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ESP32-S3-Touch-LCD-2.8C";

#define BSP_LCD_SPI_NUM SPI2_HOST
#define BSP_LCD_SPI_MOSI GPIO_NUM_1
#define BSP_LCD_SPI_SCLK GPIO_NUM_2
#define BSP_LCD_BACKLIGHT_TIMER LEDC_TIMER_0
#define BSP_LCD_BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define BSP_LCD_BACKLIGHT_RESOLUTION LEDC_TIMER_13_BIT
#define BSP_LCD_BACKLIGHT_MAX_DUTY ((1 << BSP_LCD_BACKLIGHT_RESOLUTION) - 1)
#define BSP_LCD_PIXEL_CLOCK_HZ (18 * 1000 * 1000)
#define BSP_LCD_DRAW_BUFFER_HEIGHT 40
#define BSP_LCD_INIT_QUEUE_DEPTH 1
#define BSP_LCD_BOUNCE_BUFFER_SIZE_PX (BSP_LCD_H_RES * 10)

#define BSP_LCD_RGB_HSYNC GPIO_NUM_38
#define BSP_LCD_RGB_VSYNC GPIO_NUM_39
#define BSP_LCD_RGB_DE GPIO_NUM_40
#define BSP_LCD_RGB_PCLK GPIO_NUM_41
#define BSP_LCD_RGB_D0 GPIO_NUM_5
#define BSP_LCD_RGB_D1 GPIO_NUM_45
#define BSP_LCD_RGB_D2 GPIO_NUM_48
#define BSP_LCD_RGB_D3 GPIO_NUM_47
#define BSP_LCD_RGB_D4 GPIO_NUM_21
#define BSP_LCD_RGB_D5 GPIO_NUM_14
#define BSP_LCD_RGB_D6 GPIO_NUM_13
#define BSP_LCD_RGB_D7 GPIO_NUM_12
#define BSP_LCD_RGB_D8 GPIO_NUM_11
#define BSP_LCD_RGB_D9 GPIO_NUM_10
#define BSP_LCD_RGB_D10 GPIO_NUM_9
#define BSP_LCD_RGB_D11 GPIO_NUM_46
#define BSP_LCD_RGB_D12 GPIO_NUM_3
#define BSP_LCD_RGB_D13 GPIO_NUM_8
#define BSP_LCD_RGB_D14 GPIO_NUM_18
#define BSP_LCD_RGB_D15 GPIO_NUM_17

#define BSP_PANEL_RESET_MASK IO_EXPANDER_PIN_NUM_0
#define BSP_TOUCH_RESET_MASK IO_EXPANDER_PIN_NUM_1
#define BSP_PANEL_CS_MASK IO_EXPANDER_PIN_NUM_2

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

static const lcd_init_cmd_t kLcdInitCmds[] = {
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, {0x08}, 1, 0},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, {0x3B, 0x00}, 2, 0},
    {0xC1, {0x10, 0x0C}, 2, 0},
    {0xC2, {0x07, 0x0A}, 2, 0},
    {0xC7, {0x00}, 1, 0},
    {0xCC, {0x10}, 1, 0},
    {0xCD, {0x08}, 1, 0},
    {0xB0, {0x05, 0x12, 0x98, 0x0E, 0x0F, 0x07, 0x07, 0x09, 0x09, 0x23, 0x05, 0x52, 0x0F, 0x67, 0x2C, 0x11}, 16, 0},
    {0xB1, {0x0B, 0x11, 0x97, 0x0C, 0x12, 0x06, 0x06, 0x08, 0x08, 0x22, 0x03, 0x51, 0x11, 0x66, 0x2B, 0x0F}, 16, 0},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, {0x5D}, 1, 0},
    {0xB1, {0x3E}, 1, 0},
    {0xB2, {0x81}, 1, 0},
    {0xB3, {0x80}, 1, 0},
    {0xB5, {0x4E}, 1, 0},
    {0xB7, {0x85}, 1, 0},
    {0xB8, {0x20}, 1, 0},
    {0xC1, {0x78}, 1, 0},
    {0xC2, {0x78}, 1, 0},
    {0xD0, {0x88}, 1, 0},
    {0xE0, {0x00, 0x00, 0x02}, 3, 0},
    {0xE1, {0x06, 0x30, 0x08, 0x30, 0x05, 0x30, 0x07, 0x30, 0x00, 0x33, 0x33}, 11, 0},
    {0xE2, {0x11, 0x11, 0x33, 0x33, 0xF4, 0x00, 0x00, 0x00, 0xF4, 0x00, 0x00, 0x00}, 12, 0},
    {0xE3, {0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, {0x44, 0x44}, 2, 0},
    {0xE5, {0x0D, 0xF5, 0x30, 0xF0, 0x0F, 0xF7, 0x30, 0xF0, 0x09, 0xF1, 0x30, 0xF0, 0x0B, 0xF3, 0x30, 0xF0}, 16, 0},
    {0xE6, {0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, {0x44, 0x44}, 2, 0},
    {0xE8, {0x0C, 0xF4, 0x30, 0xF0, 0x0E, 0xF6, 0x30, 0xF0, 0x08, 0xF0, 0x30, 0xF0, 0x0A, 0xF2, 0x30, 0xF0}, 16, 0},
    {0xE9, {0x36, 0x01}, 2, 0},
    {0xEB, {0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x40}, 7, 0},
    {0xED, {0xFF, 0x10, 0xAF, 0x76, 0x54, 0x2B, 0xCF, 0xFF, 0xFF, 0xFC, 0xB2, 0x45, 0x67, 0xFA, 0x01, 0xFF}, 16, 0},
    {0xEF, {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, {0}, 0, 120},
    {0x3A, {0x66}, 1, 0},
    {0x36, {0x00}, 1, 0},
    {0x35, {0x00}, 1, 0},
    {0x29, {0}, 0, 0},
};

static i2c_master_bus_handle_t s_i2c_handle = NULL;
static bool s_i2c_initialized = false;
static bool s_backlight_initialized = false;
static esp_io_expander_handle_t s_io_expander = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static lv_display_t *s_display = NULL;
static lv_indev_t *s_indev = NULL;
static uint8_t s_brightness = 0;
static esp_lv_adapter_tear_avoid_mode_t s_panel_tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
static esp_lv_adapter_rotation_t s_panel_rotation = ESP_LV_ADAPTER_ROTATE_0;

sdmmc_card_t *bsp_sdcard = NULL;

extern esp_err_t esp_lcd_touch_new_i2c_gt911(const esp_lcd_panel_io_handle_t io,
                                             const esp_lcd_touch_config_t *config,
                                             esp_lcd_touch_handle_t *out_touch);

static esp_err_t io_expander_prepare_outputs(void) {
    esp_io_expander_handle_t io = bsp_io_expander_init();
    ESP_RETURN_ON_FALSE(io != NULL, ESP_FAIL, TAG, "io expander init failed");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(io, BSP_PANEL_RESET_MASK | BSP_TOUCH_RESET_MASK | BSP_PANEL_CS_MASK,
                                IO_EXPANDER_OUTPUT),
        TAG, "set dir failed");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_level(io, BSP_PANEL_RESET_MASK | BSP_TOUCH_RESET_MASK | BSP_PANEL_CS_MASK, 1),
        TAG, "set default levels failed");
    return ESP_OK;
}

static esp_err_t panel_reset_sequence(void) {
    ESP_RETURN_ON_ERROR(io_expander_prepare_outputs(), TAG, "expander outputs");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_io_expander, BSP_PANEL_RESET_MASK, 0), TAG,
                        "panel reset low failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_io_expander, BSP_PANEL_RESET_MASK, 1), TAG,
                        "panel reset high failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t panel_write_cmd(spi_device_handle_t spi, uint8_t cmd) {
    spi_transaction_t trans = {
        .cmd = 0,
        .addr = cmd,
        .length = 0,
        .rxlength = 0,
    };
    return spi_device_transmit(spi, &trans);
}

static esp_err_t panel_write_data(spi_device_handle_t spi, uint8_t data) {
    spi_transaction_t trans = {
        .cmd = 1,
        .addr = data,
        .length = 0,
        .rxlength = 0,
    };
    return spi_device_transmit(spi, &trans);
}

static esp_err_t send_panel_init_sequence(void) {
    esp_err_t ret = ESP_OK;
    const spi_bus_config_t bus_config = {
        .mosi_io_num = BSP_LCD_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BSP_LCD_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE,
    };
    const spi_device_interface_config_t dev_config = {
        .command_bits = 1,
        .address_bits = 8,
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = BSP_LCD_INIT_QUEUE_DEPTH,
    };

    spi_device_handle_t spi = NULL;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &bus_config, SPI_DMA_CH_AUTO), TAG,
                        "spi init failed");
    ESP_GOTO_ON_ERROR(spi_bus_add_device(BSP_LCD_SPI_NUM, &dev_config, &spi), err, TAG,
                      "spi device add failed");

    ESP_GOTO_ON_ERROR(esp_io_expander_set_level(s_io_expander, BSP_PANEL_CS_MASK, 0), err, TAG,
                      "panel cs low failed");
    for (size_t i = 0; i < sizeof(kLcdInitCmds) / sizeof(kLcdInitCmds[0]); ++i) {
        const lcd_init_cmd_t *step = &kLcdInitCmds[i];
        ESP_GOTO_ON_ERROR(panel_write_cmd(spi, step->cmd), err, TAG, "cmd 0x%02x failed", step->cmd);
        for (size_t j = 0; j < step->data_len; ++j) {
            ESP_GOTO_ON_ERROR(panel_write_data(spi, step->data[j]), err, TAG,
                              "data for 0x%02x failed", step->cmd);
        }
        if (step->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(step->delay_ms));
        }
    }
    ESP_GOTO_ON_ERROR(esp_io_expander_set_level(s_io_expander, BSP_PANEL_CS_MASK, 1), err, TAG,
                      "panel cs high failed");

    spi_bus_remove_device(spi);
    spi_bus_free(BSP_LCD_SPI_NUM);
    return ESP_OK;

err:
    if (spi != NULL) {
        spi_bus_remove_device(spi);
    }
    spi_bus_free(BSP_LCD_SPI_NUM);
    return ret;
}

static lv_display_rotation_t lv_rotation_for(bsp_display_rotation_t rotation) {
    switch (rotation) {
        case BSP_DISPLAY_ROTATE_90:
            return LV_DISPLAY_ROTATION_90;
        case BSP_DISPLAY_ROTATE_180:
            return LV_DISPLAY_ROTATION_180;
        case BSP_DISPLAY_ROTATE_270:
            return LV_DISPLAY_ROTATION_270;
        case BSP_DISPLAY_ROTATE_0:
        default:
            return LV_DISPLAY_ROTATION_0;
    }
}

static esp_lv_adapter_tear_avoid_mode_t resolve_tear_mode(
    esp_lv_adapter_tear_avoid_mode_t requested_mode) {
    if (requested_mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC) {
        ESP_LOGW(TAG,
                 "TE sync not available on the 2.8C RGB panel, falling back to single framebuffer mode");
        return ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    }
    return requested_mode;
}

static esp_err_t apply_touch_flags(const bsp_display_cfg_t *cfg) {
    if (s_touch_handle == NULL || cfg == NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_swap_xy(s_touch_handle, cfg->touch_flags.swap_xy != 0), TAG,
                        "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_mirror_x(s_touch_handle, cfg->touch_flags.mirror_x != 0), TAG,
                        "mirror_x failed");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_set_mirror_y(s_touch_handle, cfg->touch_flags.mirror_y != 0), TAG,
                        "mirror_y failed");
    return ESP_OK;
}

esp_err_t bsp_i2c_init(void) {
    if (s_i2c_initialized) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BSP_I2C_NUM,
        .scl_io_num = BSP_I2C_SCL,
        .sda_io_num = BSP_I2C_SDA,
        .glitch_ignore_cnt = 7,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &s_i2c_handle), TAG, "i2c init failed");
    s_i2c_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void) {
    if (!s_i2c_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(i2c_del_master_bus(s_i2c_handle), TAG, "i2c deinit failed");
    s_i2c_handle = NULL;
    s_i2c_initialized = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void) {
    if (bsp_i2c_init() != ESP_OK) {
        return NULL;
    }
    return s_i2c_handle;
}

esp_io_expander_handle_t bsp_io_expander_init(void) {
    if (s_io_expander != NULL) {
        return s_io_expander;
    }
    if (bsp_i2c_init() != ESP_OK) {
        return NULL;
    }
    if (esp_io_expander_new_i2c_tca9554(s_i2c_handle, BSP_IO_EXPANDER_I2C_ADDRESS,
                                        &s_io_expander) != ESP_OK) {
        return NULL;
    }
    return s_io_expander;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config,
                          esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io) {
    (void)config;
    if (s_panel_handle != NULL) {
        if (ret_panel != NULL) {
            *ret_panel = s_panel_handle;
        }
        if (ret_io != NULL) {
            *ret_io = NULL;
        }
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(panel_reset_sequence(), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(send_panel_init_sequence(), TAG, "panel init sequence failed");

    const uint8_t required_frame_buffers =
        esp_lv_adapter_get_required_frame_buffer_count(s_panel_tear_mode, s_panel_rotation);

    const esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings =
            {
                .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
                .h_res = BSP_LCD_H_RES,
                .v_res = BSP_LCD_V_RES,
                .hsync_back_porch = 10,
                .hsync_front_porch = 50,
                .hsync_pulse_width = 8,
                .vsync_back_porch = 18,
                .vsync_front_porch = 8,
                .vsync_pulse_width = 2,
                .flags =
                    {
                        .pclk_active_neg = false,
                    },
            },
        .data_width = 16,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .num_fbs = required_frame_buffers,
        .bounce_buffer_size_px = BSP_LCD_BOUNCE_BUFFER_SIZE_PX,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = BSP_LCD_RGB_HSYNC,
        .vsync_gpio_num = BSP_LCD_RGB_VSYNC,
        .de_gpio_num = BSP_LCD_RGB_DE,
        .pclk_gpio_num = BSP_LCD_RGB_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums =
            {
                BSP_LCD_RGB_D0,
                BSP_LCD_RGB_D1,
                BSP_LCD_RGB_D2,
                BSP_LCD_RGB_D3,
                BSP_LCD_RGB_D4,
                BSP_LCD_RGB_D5,
                BSP_LCD_RGB_D6,
                BSP_LCD_RGB_D7,
                BSP_LCD_RGB_D8,
                BSP_LCD_RGB_D9,
                BSP_LCD_RGB_D10,
                BSP_LCD_RGB_D11,
                BSP_LCD_RGB_D12,
                BSP_LCD_RGB_D13,
                BSP_LCD_RGB_D14,
                BSP_LCD_RGB_D15,
            },
        .flags =
            {
                .fb_in_psram = true,
            },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_config, &s_panel_handle), TAG,
                        "new rgb panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "panel init failed");

    if (ret_panel != NULL) {
        *ret_panel = s_panel_handle;
    }
    if (ret_io != NULL) {
        *ret_io = NULL;
    }
    return ESP_OK;
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg) {
    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(s_panel_handle, NULL, BSP_LCD_H_RES, BSP_LCD_V_RES,
                                                  cfg->rotation);
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = resolve_tear_mode(cfg->tear_avoid_mode);
    disp_cfg.profile.use_psram = true;
    disp_cfg.profile.buffer_height = BSP_LCD_DRAW_BUFFER_HEIGHT;
    disp_cfg.profile.require_double_buffer = false;
    disp_cfg.tear_avoid_mode = tear_mode;
    disp_cfg.te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED();
    return esp_lv_adapter_register_display(&disp_cfg);
}

static lv_indev_t *bsp_display_indev_init(const bsp_display_cfg_t *cfg, lv_display_t *disp) {
    const esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = 0x5D,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags =
            {
                .disable_control_phase = 1,
            },
        .scl_speed_hz = 400000,
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels =
            {
                .reset = 0,
                .interrupt = 0,
            },
        .flags =
            {
                .swap_xy = cfg->touch_flags.swap_xy,
                .mirror_x = cfg->touch_flags.mirror_x,
                .mirror_y = cfg->touch_flags.mirror_y,
            },
    };

    ESP_RETURN_ON_FALSE(bsp_i2c_get_handle() != NULL, NULL, TAG, "i2c handle unavailable");
    if (esp_lcd_new_panel_io_i2c(s_i2c_handle, &tp_io_config, &tp_io) != ESP_OK) {
        return NULL;
    }
    if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch_handle) != ESP_OK) {
        return NULL;
    }

    const esp_lv_adapter_touch_config_t touch_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_touch_handle);
    return esp_lv_adapter_register_touch(&touch_cfg);
}

lv_display_t *bsp_display_start(void) {
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL,
        .touch_flags =
            {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
    };
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg) {
    ESP_RETURN_ON_FALSE(cfg != NULL, NULL, TAG, "cfg is null");
    if (s_display != NULL) {
        return s_display;
    }

    s_panel_tear_mode = resolve_tear_mode(cfg->tear_avoid_mode);
    s_panel_rotation = cfg->rotation;

    if (esp_lv_adapter_init(&cfg->lv_adapter_cfg) != ESP_OK) {
        return NULL;
    }
    if (bsp_display_new(NULL, NULL, NULL) != ESP_OK) {
        return NULL;
    }
    s_display = bsp_display_lcd_init(cfg);
    if (s_display == NULL) {
        return NULL;
    }
    s_indev = bsp_display_indev_init(cfg, s_display);
    if (s_indev == NULL) {
        return NULL;
    }
    if (bsp_display_brightness_init() != ESP_OK) {
        return NULL;
    }
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    (void)apply_touch_flags(cfg);
    return s_display;
}

lv_indev_t *bsp_display_get_input_dev(void) {
    return s_indev;
}

esp_err_t bsp_display_rotation_set(bsp_display_rotation_t rotation) {
    if (s_display == NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_lv_adapter_lock(1000), TAG, "display lock failed");
    lv_display_set_rotation(s_display, lv_rotation_for(rotation));
    esp_lv_adapter_unlock();
    return ESP_OK;
}

esp_err_t bsp_display_lock(uint32_t timeout_ms) {
    return esp_lv_adapter_lock((int32_t)timeout_ms);
}

void bsp_display_unlock(void) {
    esp_lv_adapter_unlock();
}

static uint32_t backlight_duty_for_percent(int brightness_percent) {
    const int clamped = brightness_percent < 0 ? 0 : (brightness_percent > 100 ? 100 : brightness_percent);
    if (clamped == 0) {
        return 0;
    }
    return BSP_LCD_BACKLIGHT_MAX_DUTY - (81U * (100U - (uint32_t)clamped));
}

esp_err_t bsp_display_brightness_init(void) {
    if (s_backlight_initialized) {
        return ESP_OK;
    }
    const gpio_config_t gpio_config_backlight = {
        .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_config_backlight), TAG, "backlight gpio config failed");

    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = BSP_LCD_BACKLIGHT_TIMER,
        .duty_resolution = BSP_LCD_BACKLIGHT_RESOLUTION,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "backlight timer config failed");

    const ledc_channel_config_t channel_config = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BSP_LCD_BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BSP_LCD_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), TAG, "backlight channel config failed");

    s_backlight_initialized = true;
    return bsp_display_brightness_set(70);
}

esp_err_t bsp_display_brightness_set(int brightness_percent) {
    ESP_RETURN_ON_FALSE(s_backlight_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "backlight not initialized");
    const int clamped = brightness_percent < 0 ? 0 : (brightness_percent > 100 ? 100 : brightness_percent);
    s_brightness = (uint8_t)clamped;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_BACKLIGHT_CHANNEL,
                                      backlight_duty_for_percent(clamped)),
                        TAG, "set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_BACKLIGHT_CHANNEL), TAG,
                        "update duty failed");
    return ESP_OK;
}

int bsp_display_brightness_get(void) {
    return s_brightness;
}

esp_err_t bsp_display_backlight_on(void) {
    return bsp_display_brightness_set(100);
}

esp_err_t bsp_display_backlight_off(void) {
    return bsp_display_brightness_set(0);
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void) {
    ESP_LOGW(TAG, "Audio codec is not ported on the 2.8C board yet");
    return NULL;
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void) {
    return NULL;
}

esp_err_t bsp_audio_init(const void *i2s_config) {
    (void)i2s_config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_spiffs_mount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_spiffs_unmount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_sdcard_mount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_sdcard_unmount(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

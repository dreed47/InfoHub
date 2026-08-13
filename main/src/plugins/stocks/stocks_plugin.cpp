#include "infohub/plugins/stocks/stocks_plugin.hpp"

#include <cstdio>
#include <cstdlib>

#include "esp_log.h"
#include "infohub/ui.hpp"
#include "infohub/ui_toolkit.hpp"

#if defined(INFOHUB_HW_VARIANT_AMOLED_1_75)
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#elif defined(INFOHUB_HW_VARIANT_LCD_2_8C)
#include "bsp/esp32_s3_touch_lcd_2_8c.h"
#else
#error "Unknown InfoHub hardware variant"
#endif

// Same fonts weather_plugin.cpp uses for its own pages -- plain C objects,
// declared the same way, extern "C" at file scope (not inside namespace
// infohub, which would give them C++ mangled names that don't match the
// real symbols).
extern "C" {
extern const lv_font_t dosis_20;
extern const lv_font_t dosis_32;
}

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.stocks";
constexpr char kPluginNs[] = "stocks";
constexpr uint32_t kGreenHex = 0x00CC66;
constexpr uint32_t kRedHex = 0xFF3333;
constexpr uint32_t kNeutralHex = 0x666666;
}  // namespace

esp_err_t StocksPlugin::init(PluginContext& ctx) {
  config_store_ = &ctx.config_store;
  wifi_manager_ = &ctx.wifi_manager;
  setup_portal_ = &ctx.setup_portal;
  ui_ = &ctx.ui;

  client_.set_wifi_manager(wifi_manager_);
  client_.set_network_arbiter(&ctx.network_arbiter);
  load_config();

  const esp_err_t err = client_.start();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "StockClient task start failed: %s", esp_err_to_name(err));
  }
  return err;
}

void StocksPlugin::load_config() {
  if (config_store_ == nullptr) {
    return;
  }
  std::array<std::string, kStockSymbolCount> symbols{};
  for (uint8_t i = 0; i < kStockSymbolCount; ++i) {
    const std::string key = "symbol" + std::to_string(i + 1);
    symbols[i] = config_store_->load_plugin_string(kPluginNs, key.c_str());
  }
  const std::string api_key = config_store_->load_plugin_string(kPluginNs, "api_token");
  const std::string poll_s_str = config_store_->load_plugin_string(kPluginNs, "poll_s");
  uint32_t poll_interval_s = 21600;
  if (!poll_s_str.empty()) {
    poll_interval_s = static_cast<uint32_t>(std::strtoul(poll_s_str.c_str(), nullptr, 10));
  }
  client_.configure(symbols, api_key, poll_interval_s);

  set_enabled(config_store_->load_plugin_string(kPluginNs, "enabled") != "0");
}

void StocksPlugin::tick(uint64_t) {
  // No per-tick work -- polling happens entirely on StockClient's own task;
  // update_ui() (called every Application loop iteration regardless of
  // this) is where the fetched snapshot gets pushed to the screen.
}

bool StocksPlugin::wants_network() const {
  return client_.snapshot().configured;
}

void StocksPlugin::build_screen(lv_obj_t* parent, uint8_t /*page_index*/) {
  if (parent == nullptr) {
    return;
  }

  LvglLockGuard lock(3000, "StocksPlugin::build_screen");
  if (!lock.locked()) {
    return;
  }

  lv_obj_t* column = lv_obj_create(parent);
  lv_obj_set_size(column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(column, 0, 0);
  lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(column, 14, 0);
  lv_obj_center(column);

  for (uint8_t i = 0; i < kStockSymbolCount; ++i) {
    lv_obj_t* row = lv_obj_create(column);
    lv_obj_set_size(row, 300, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);

    QuoteRow& qr = rows_[i];

    qr.symbol_label = lv_label_create(row);
    lv_label_set_text(qr.symbol_label, "--");
    lv_obj_set_style_text_font(qr.symbol_label, &dosis_32, 0);
    lv_obj_set_style_text_color(qr.symbol_label, lv_color_hex(0xFFFFFF), 0);

    qr.price_label = lv_label_create(row);
    lv_label_set_text(qr.price_label, "--");
    lv_obj_set_style_text_font(qr.price_label, &dosis_32, 0);
    lv_obj_set_style_text_color(qr.price_label, lv_color_hex(0xCCCCCC), 0);

    qr.dot = lv_obj_create(row);
    lv_obj_set_size(qr.dot, 16, 16);
    lv_obj_set_style_radius(qr.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(qr.dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(qr.dot, lv_color_hex(kNeutralHex), 0);
    lv_obj_set_style_border_width(qr.dot, 0, 0);
    lv_obj_clear_flag(qr.dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(qr.dot, LV_OBJ_FLAG_CLICKABLE);
  }

  status_label_ = lv_label_create(column);
  lv_label_set_text(status_label_, "Not configured");
  lv_obj_set_width(status_label_, 320);
  lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(status_label_, &dosis_20, 0);
  lv_obj_set_style_text_color(status_label_, lv_color_hex(0x999999), 0);
}

void StocksPlugin::update_ui() {
  if (ui_ == nullptr || rows_[0].symbol_label == nullptr) {
    return;
  }

  const StockSnapshot snapshot = client_.snapshot();
  ui_->set_plugin_pages_enabled(id(), snapshot.configured);
  if (!snapshot.configured) {
    return;
  }

  LvglLockGuard lock(200, "StocksPlugin::update_ui");
  if (!lock.locked()) {
    return;
  }

  bool any_data = false;
  for (uint8_t i = 0; i < kStockSymbolCount; ++i) {
    const StockQuote& quote = snapshot.quotes[i];
    QuoteRow& qr = rows_[i];
    if (quote.symbol.empty()) {
      lv_label_set_text(qr.symbol_label, "--");
      lv_label_set_text(qr.price_label, "");
      lv_obj_set_style_bg_color(qr.dot, lv_color_hex(kNeutralHex), 0);
      continue;
    }
    lv_label_set_text(qr.symbol_label, quote.symbol.c_str());
    if (quote.has_data) {
      any_data = true;
      char price_buf[24];
      std::snprintf(price_buf, sizeof(price_buf), "%.2f", quote.price);
      lv_label_set_text(qr.price_label, price_buf);
      lv_obj_set_style_bg_color(qr.dot, lv_color_hex(quote.change >= 0.0 ? kGreenHex : kRedHex), 0);
    } else {
      lv_label_set_text(qr.price_label, "--");
      lv_obj_set_style_bg_color(qr.dot, lv_color_hex(kNeutralHex), 0);
    }
  }

  if (any_data) {
    lv_label_set_text(status_label_,
                      snapshot.last_fetch_ok ? "" : "Last update failed - showing stale data");
  } else {
    lv_label_set_text(status_label_,
                      snapshot.last_error.empty() ? "Waiting for first fetch..."
                                                   : snapshot.last_error.c_str());
  }
}

}  // namespace infohub

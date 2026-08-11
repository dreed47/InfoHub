#pragma once

#include <string>

#include "lvgl.h"

namespace printsphere {

// Small generic LVGL widget-building helpers shared between Ui and plugin
// content code (e.g. PrinterPlugin's own screens). Kept deliberately tiny —
// only what has more than one real consumer today.
void make_transparent(lv_obj_t* obj);
void enable_touch_bubble(lv_obj_t* obj);
void set_hidden(lv_obj_t* obj, bool hidden);
void set_label_text_if_changed(lv_obj_t* label, const char* text);
void set_label_text_if_changed(lv_obj_t* label, const std::string& text);

}  // namespace printsphere

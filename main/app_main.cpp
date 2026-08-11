#include "infohub/application.hpp"

#include <cassert>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef INFOHUB_DEBUG_BUILD
#include "infohub/debug_log_buffer.hpp"
#endif

namespace {

constexpr uint32_t kApplicationTaskStackBytes = 12288;
constexpr UBaseType_t kApplicationTaskPriority = 5;

void application_task(void*) {
  static infohub::Application app;
  app.run();
  vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main(void) {
#ifdef INFOHUB_DEBUG_BUILD
  infohub::debug_log_init();
#endif
  const BaseType_t created =
      xTaskCreate(application_task, "infohub_app", kApplicationTaskStackBytes / sizeof(StackType_t),
                  nullptr, kApplicationTaskPriority, nullptr);
  assert(created == pdPASS);
}

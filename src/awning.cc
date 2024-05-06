#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "FreeRTOS.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/printf.h"
#include "portmacro.h"
#include "projdefs.h"
#include "task.h"

namespace jagsawning {

constexpr int kPinButtonUp = 20;
constexpr int kPinButtonMy = 19;
constexpr int kPinButtonDn = 18;

constexpr uint8_t kButtonUpShift = 0;
constexpr uint8_t kButtonMyShift = 1;
constexpr uint8_t kButtonDnShift = 2;
constexpr uint8_t kButtonProgMask =
    (1 << kButtonUpShift) | (1 << kButtonDnShift);

uint8_t GetPressedButtons() {
  return (static_cast<uint8_t>(!gpio_get(kPinButtonUp)) << kButtonUpShift) |
         (static_cast<uint8_t>(!gpio_get(kPinButtonMy)) << kButtonMyShift) |
         (static_cast<uint8_t>(!gpio_get(kPinButtonDn)) << kButtonDnShift);
}

bool IsUpPressed(uint8_t buttons) { return buttons & (1 << kButtonUpShift); }
bool IsMyPressed(uint8_t buttons) { return buttons & (1 << kButtonMyShift); }
bool IsDnPressed(uint8_t buttons) { return buttons & (1 << kButtonDnShift); }

void button_task(void *) {
  for (int pin : {kPinButtonUp, kPinButtonMy, kPinButtonDn}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
  }

  static TaskHandle_t task_handle = xTaskGetCurrentTaskHandle();
  gpio_set_irq_callback(+[](uint gpio, uint32_t event_mask) {
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(task_handle, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
  });
  gpio_set_irq_enabled(kPinButtonDn, GPIO_IRQ_EDGE_FALL, true);
  gpio_set_irq_enabled(kPinButtonUp, GPIO_IRQ_EDGE_FALL, true);
  gpio_set_irq_enabled(kPinButtonMy, GPIO_IRQ_EDGE_FALL, true);
  while (true) {
    xTaskNotifyWait(0b1, 0b1, nullptr, portMAX_DELAY);

    constexpr int kMsToCheck = 150;
    constexpr int kDelayMs = 25;
    constexpr int kTimesToCheck = kMsToCheck / kDelayMs;

    uint8_t pressed = GetPressedButtons();
    for (int i = 0; i < kTimesToCheck; ++i) {
      // Wait for the pressed buttons to be stable for
      vTaskDelay(pdMS_TO_TICKS(kDelayMs));
      uint8_t newpressed = GetPressedButtons();
      if (newpressed == 0) {
        pressed = 0;
        break;
      } else if (newpressed != pressed) {
        pressed = newpressed;
      }
    }

    if (IsUpPressed(pressed) && IsDnPressed(pressed)) {
      printf("prog\n");
    } else if (IsUpPressed(pressed)) {
      printf("up\n");
    } else if (IsMyPressed(pressed)) {
      printf("my\n");
    } else if (IsDnPressed(pressed)) {
      printf("dn\n");
    }

    while (GetPressedButtons() != 0) {
      vTaskDelay(pdMS_TO_TICKS(kDelayMs));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void main_task() {
  xTaskCreate(button_task, "button_task", 512, nullptr, 1, nullptr);

  vTaskDelay(portMAX_DELAY);
}

}  // namespace jagsawning

extern "C" void main_task(void *) { jagsawning::main_task(); }
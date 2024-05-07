#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "FreeRTOS.h"
#include "SomfyRemote.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/printf.h"
#include "pico/time.h"
#include "pico/types.h"
#include "pico_rolling_code_storage.h"
#include "portmacro.h"
#include "projdefs.h"
#include "queue.h"
#include "rfm69.h"
#include "task.h"

namespace jagsawning {
namespace {
constexpr int kPinButtonUp = 20;
constexpr int kPinButtonMy = 19;
constexpr int kPinButtonDn = 18;
constexpr int kPinRadioDio2 = 15;

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

struct CommandMessage {
  // What was the somfy command?
  Command command;

  // Was the command generated manually via a button press?
  bool manual;
};

// Returns the CommandQueue used for passing from the command-generator tasks
// to the radio task.
static QueueHandle_t CommandQueue() {
  static QueueHandle_t queue = xQueueCreate(1, sizeof(CommandMessage));
  return queue;
}

// We don't worry about dropping commands because if it's user-generated then
// the user can just press the button again and if it's automatically-generated
// it's in conflict with the user command anyway.
void TryPushCommand(const CommandMessage &m) {
  xQueueSend(CommandQueue(), &m, 0);
}

// Returns the next command pushed to the queue. Blocks forever until a command
// is pushed.
void GetCommandBlocking(CommandMessage &m) {
  while (xQueueReceive(CommandQueue(), &m, portMAX_DELAY) != pdTRUE) {
  }
}

// Drains the command queue of any pending command. We throw away any commands
// we received while we were transmitting a radio message before blocking.
void DrainCommandQueue() {
  CommandMessage m;
  while (xQueueReceive(CommandQueue(), nullptr, 0) == pdTRUE) {
  }
}

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
  printf("Button IRQs enabled\n");
  while (true) {
    xTaskNotifyWait(0b1, 0b1, nullptr, portMAX_DELAY);

    printf("Woke up button task\n");

    constexpr int kMsToCheck = 150;
    constexpr int kDelayMs = 25;
    constexpr int kTimesToCheck = kMsToCheck / kDelayMs;

    uint8_t pressed = GetPressedButtons();
    printf("Pressed %0hhx\n", pressed);
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

    CommandMessage message{.manual = true};
    if (IsUpPressed(pressed) && IsDnPressed(pressed)) {
      message.command = Command::Prog;
    } else if (IsUpPressed(pressed)) {
      message.command = Command::Up;
    } else if (IsMyPressed(pressed)) {
      message.command = Command::My;
    } else if (IsDnPressed(pressed)) {
      message.command = Command::Down;
    } else {
      printf("unexpected button combination, skipping\n");
      continue;
    }
    printf("Buttons: %02hhx\n", pressed);
    TryPushCommand(message);

    while (GetPressedButtons() != 0) {
      vTaskDelay(pdMS_TO_TICKS(kDelayMs));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

#ifndef SOMFY_RADIO_ADDRESS
#error SOMFY_RADIO_ADDRESS must be defined.
#endif

void radio_task(void *) {
  RfmSpiDriver driver = RfmSpiDriver::Create();
  driver.SetMode(RfmSpiDriver::kSleep);
  driver.SetCarrierFrequency(433'420'000);
  driver.SetModulationType(RfmSpiDriver::kOok);
  driver.SetDataMode(RfmSpiDriver::kContinuousWithoutSynchronizer);
  driver.SetPower(0b1111'1111);

  PicoFlashRCS storage;
  SomfyRemote remote(kPinRadioDio2, SOMFY_RADIO_ADDRESS, &storage);

  // Radio is ready. Wait for a command.
  // Note that we're recording the current time as the last message time because
  absolute_time_t last_message_time = nil_time;
  CommandMessage last_message{Command::My, false};
  while (true) {
    CommandMessage message;
    printf("Waiting for command\n");
    GetCommandBlocking(message);
    printf(
        "Received command %hhu manual=%d\n",
        static_cast<byte>(message.command),
        message.manual);

    absolute_time_t receive_time = get_absolute_time();

    // Discard any automatic messages if a manual message has been received in
    // the last hour.
    if (last_message.manual && !message.manual &&
        (is_nil_time(last_message_time) ||
         absolute_time_diff_us(last_message_time, receive_time) <
             (3600ll * 1'000'000))) {
      printf("Automatic command received too recently after manual command\n");
      continue;
    }

    // We will send this command. Turn on the radio and wait for it to become
    // ready.
    driver.SetMode(RfmSpiDriver::kTransmit);
    printf("Waiting for tx ready.\n");
    while (!(driver.GetRegIrqFlags() & RfmSpiDriver::kTxReady)) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    printf("Sending command:%hhu\n", static_cast<byte>(message.command));
    remote.sendCommand(message.command);

    driver.SetMode(RfmSpiDriver::kSleep);
    last_message = message;
    last_message_time = receive_time;
    DrainCommandQueue();

    printf("command sent, waiting for next command\n");
  }
}

void create_tasks() {
  xTaskCreateAffinitySet(
      button_task, "button_task", 512, nullptr, 1, 0b01, nullptr);
  xTaskCreate(radio_task, "radio_task", 512, nullptr, 2, nullptr);
  vTaskDelete(nullptr);
}

}  // namespace
}  // namespace jagsawning

extern "C" void main_task(void *) { jagsawning::create_tasks(); }
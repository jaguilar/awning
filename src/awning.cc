#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <type_traits>

#include "FreeRTOS.h"
#include "SomfyRemote.h"
#include "event_groups.h"
#include "freertosxx/event.h"
#include "freertosxx/mutex.h"
#include "freertosxx/queue.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/regs/intctrl.h"
#include "homeassistant_service.h"
#include "lwip/api.h"
#include "lwip/apps/mqtt.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/netbuf.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "pico/printf.h"
#include "pico/time.h"
#include "pico/types.h"
#include "pico_rolling_code_storage.h"
#include "portmacro.h"
#include "projdefs.h"
#include "queue.h"
#include "rfm69.h"
#include "semphr.h"
#include "task.h"

namespace jagsawning {
namespace {
// Combines the radio and the somfy remote class into a single controller
// object.
class SomfyController {
 public:
  static constexpr int kDataPin = 15;

  static std::unique_ptr<SomfyController> Create() {
    RfmSpiDriver driver = RfmSpiDriver::Create();
    driver.SetMode(RfmSpiDriver::kSleep);
    driver.SetCarrierFrequency(433'420'000);
    driver.SetModulationType(RfmSpiDriver::kOok);
    driver.SetDataMode(RfmSpiDriver::kContinuousWithoutSynchronizer);
    driver.SetPower(0b1111'1111);

    sleep_ms(100);  // Give the radio time to come up.

    gpio_init(kDataPin);
    gpio_set_dir(kDataPin, true);

    return std::unique_ptr<SomfyController>(
        new SomfyController(std::move(driver)));
  }

  // Yeah, Command and SomfyRemote are not namespaced. :(
  void SendCommand(::Command c) {
    printf("Waiting for transmit ready\n");
    rfm_driver_.SetMode(RfmSpiDriver::kTransmit);
    while (!(rfm_driver_.GetRegIrqFlags() & RfmSpiDriver::kTxReady)) {
      sleep_ms(100);
    }
    printf("Transmit ready\n");
    remote_.sendCommand(c, 3);
    printf("Transmit done, sleeping\n");
    rfm_driver_.SetMode(RfmSpiDriver::kSleep);
  }

 private:
  SomfyController(RfmSpiDriver rfm_driver) : rfm_driver_(rfm_driver) {
    remote_.setup();
  }

  RfmSpiDriver rfm_driver_;
  PicoFlashRCS storage_;
  SomfyRemote remote_{kDataPin, SOMFY_RADIO_ADDRESS, &storage_};
};
std::unique_ptr<SomfyController> g_somfy_controller = nullptr;
freertosxx::Mutex g_controller_mutex;

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
  ::Command command;
};

// Returns the CommandQueue used for passing from the command-generator tasks
// to the radio task.
static freertosxx::Queue<CommandMessage>& CommandQueue() {
  static freertosxx::StaticQueue<CommandMessage, 1> queue;
  return queue;
}

void button_task(void *) {
  for (int pin : {kPinButtonUp, kPinButtonMy, kPinButtonDn}) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
  }

  static TaskHandle_t task_handle = xTaskGetCurrentTaskHandle();
  taskENTER_CRITICAL();
  gpio_set_irq_callback(+[](uint gpio, uint32_t event_mask) {
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(task_handle, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
  });
  gpio_set_irq_enabled(kPinButtonDn, GPIO_IRQ_EDGE_FALL, true);
  gpio_set_irq_enabled(kPinButtonUp, GPIO_IRQ_EDGE_FALL, true);
  gpio_set_irq_enabled(kPinButtonMy, GPIO_IRQ_EDGE_FALL, true);
  irq_set_enabled(IO_IRQ_BANK0, true);
  taskEXIT_CRITICAL();
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

    ::Command command;
    if (IsUpPressed(pressed) && IsDnPressed(pressed)) {
      command = Command::Prog;
    } else if (IsUpPressed(pressed)) {
      command = Command::Up;
    } else if (IsMyPressed(pressed)) {
      command = Command::My;
    } else if (IsDnPressed(pressed)) {
      command = Command::Down;
    } else {
      printf("unexpected button combination, skipping\n");
      continue;
    }
    printf("Buttons: %02hhx\n", pressed);
    {
      // Note that we explicitly do not update the state of the awning
      // on homeassistant here, since this is morally equivalent to a
      // button being pressed on a different remote, which we can't observe.
      // I mean we could I guess but I would have to decode somfy commands
      // and that seems like a lot of work.
      freertosxx::MutexLock lock(g_controller_mutex);
      g_somfy_controller->SendCommand(command);
    }

    while (GetPressedButtons() != 0) {
      vTaskDelay(pdMS_TO_TICKS(kDelayMs));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

freertosxx::StaticEventGroup command_event;

void homeassistant_task(void*) {
  std::expected<std::unique_ptr<MqttClientWrapper>, err_t> client =
      MqttClientWrapper::Create(MQTT_HOST, MQTT_PORT, ConnectInfoFromDefines());
  if (!client) {
    panic(
        "failed to connsect to %s due to error %d\n",
        MQTT_HOST,
        client.error());
  }

  freertosxx::EventGroup eg;

  std::expected<std::unique_ptr<HomeAssistantService>, err_t> service =
      HomeAssistantService::Connect(
          **client, [&](HomeAssistantService::Command cmd) {
            printf("Received command %d\n", static_cast<int>(cmd));
            command_event.Clear(
                HomeAssistantService::kCmdClose |
                HomeAssistantService::kCmdOpen |
                HomeAssistantService::kCmdStop);
            command_event.Set(cmd);
          });
  if (!service) {
    panic(
        "failed to connect to home assistant: %d\n",
        lwip_strerr(service.error()));
  }

  HomeAssistantService::State current_state = HomeAssistantService::kClosed;
  HomeAssistantService::State dest_state = HomeAssistantService::kClosed;
  absolute_time_t time_when_state_change_done = at_the_end_of_time;

  while (true) {
    std::optional<TickType_t> timeout;
    if (!is_at_the_end_of_time(time_when_state_change_done)) {
      const int64_t ms_til_timeout =
          absolute_time_diff_us(
              get_absolute_time(), time_when_state_change_done) /
          1000;
      if (ms_til_timeout > 0) {
        timeout = pdMS_TO_TICKS(ms_til_timeout);
      }
    }

    constexpr int kCoverCloseOpenTime = 8000;

    // Wait for a command to be received.
    EventBits_t event = command_event.Wait(
        HomeAssistantService::kCmdClose | HomeAssistantService::kCmdOpen |
            HomeAssistantService::kCmdStop,
        {.clear = true, .timeout = timeout});
    printf("recieved %d\n", event);
    std::optional<::Command> command;
    if (event == 0) {
      // Timed out.
      current_state = dest_state;
      time_when_state_change_done = at_the_end_of_time;
    } else if (event & HomeAssistantService::kCmdStop) {
      // Sad story: if we send the stop command while the cover isn't actually
      // moving it will open. This isn't good but I don't know of any way to
      // address it.
      command = ::Command::My;
      current_state = dest_state = HomeAssistantService::kStopped;
      time_when_state_change_done = at_the_end_of_time;
    } else if (event & HomeAssistantService::kCmdOpen) {
      // In my deployment, what we want isn't full-open. The "My" command is
      // programmed to stop the awning at the right spot.
      // 
      // Bug: if the cover is currently closing, pressing "My" will simply stop
      // it from closing. We don't have any true feedback from the cover to know
      // if it is closing or not, so we just have to hope. The less bad thing
      // would be to stop it closing, as opposed to another bad thing that could
      // happen which would be to open it.
      command = ::Command::My;
      current_state = HomeAssistantService::kOpening;
      dest_state = HomeAssistantService::kOpen;
      time_when_state_change_done =
          delayed_by_ms(get_absolute_time(), kCoverCloseOpenTime);
    } else {
      assert(event & HomeAssistantService::kCmdClose);
      command = ::Command::Up;
      current_state = HomeAssistantService::kClosing;
      dest_state = HomeAssistantService::kClosed;
      time_when_state_change_done =
          delayed_by_ms(get_absolute_time(), kCoverCloseOpenTime);
    }

    (*service)->SetState(current_state);
    if (command) {
      freertosxx::MutexLock lock(g_controller_mutex);
      g_somfy_controller->SendCommand(*command);
    }
  }
}

void create_tasks() {
  g_somfy_controller = SomfyController::Create();
  xTaskCreate(button_task, "button_task", 512, nullptr, 1, nullptr);
  xTaskCreate(
      homeassistant_task, "homeassistant_task", 512, nullptr, 2, nullptr);
  vTaskDelete(nullptr);
}

}  // namespace
}  // namespace jagsawning

extern "C" void main_task(void*) { jagsawning::create_tasks(); }
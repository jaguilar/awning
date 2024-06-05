#include "homeassistant_service.h"

#include <cstdio>
#include <expected>

#include "freertosxx/event.h"
#include "freertosxx/mutex.h"
#include "freertosxx/queue.h"
#include "pico/time.h"
#include "pico/types.h"
#include "portmacro.h"
#include "projdefs.h"

namespace jagsawning {

freertosxx::StaticEventGroup command_event;
HomeAssistantService* homeassisant_cover = nullptr;

void fake_awning_task(void*) {
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
      timeout = pdMS_TO_TICKS(ms_til_timeout);
    }

    // Wait for a command to be received.
    EventBits_t event = command_event.Wait(
        HomeAssistantService::kCmdClose | HomeAssistantService::kCmdOpen |
            HomeAssistantService::kCmdStop,
        {.clear = true, .timeout = timeout});
    printf("recieved %d\n", event);
    if (event == 0) {
      // Timed out.
      current_state = dest_state;
      time_when_state_change_done = at_the_end_of_time;
    } else if (event & HomeAssistantService::kCmdStop) {
      current_state = dest_state = HomeAssistantService::kStopped;
      time_when_state_change_done = at_the_end_of_time;
    } else if (event & HomeAssistantService::kCmdOpen) {
      current_state = HomeAssistantService::kOpening;
      dest_state = HomeAssistantService::kOpen;
      time_when_state_change_done = delayed_by_ms(get_absolute_time(), 5000);
    } else {
      assert(event & HomeAssistantService::kCmdClose);
      current_state = HomeAssistantService::kClosing;
      dest_state = HomeAssistantService::kClosed;
      time_when_state_change_done = delayed_by_ms(get_absolute_time(), 5000);
    }

    sleep_ms(500);  // Simulate network delay.
    homeassisant_cover->SetState(current_state);
  }
}

void DoTest() {
  std::expected<std::unique_ptr<MqttClientWrapper>, err_t> client =
      MqttClientWrapper::Create(MQTT_HOST, MQTT_PORT, ConnectInfoFromDefines());
  if (!client) {
    printf(
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
  homeassisant_cover = service->get();

  xTaskCreate(fake_awning_task, "fake_awning_task", 512, nullptr, 1, nullptr);

  while (true) {
    // Wait for commands.
    sleep_ms(20000);
  }
}

}  // namespace jagsawning

extern "C" void main_task(void*) { awning::DoTest(); }
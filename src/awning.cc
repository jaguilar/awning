#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <type_traits>

#include "FreeRTOS.h"
#include "SomfyRemote.h"
#include "event_groups.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/regs/intctrl.h"
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
  while (xQueueReceive(CommandQueue(), &m, 0) == pdTRUE) {
  }
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

class RTOSQueueBase {
 public:
  RTOSQueueBase(QueueHandle_t queue) : queue_(queue) {}
  ~RTOSQueueBase() { vQueueDelete(queue_); }
  RTOSQueueBase(const RTOSQueueBase &) = delete;
  RTOSQueueBase(RTOSQueueBase &&) = delete;
  RTOSQueueBase &operator=(const RTOSQueueBase &) = delete;
  RTOSQueueBase &operator=(RTOSQueueBase &&) = delete;

  void Drain() {}

 protected:
  void SendUntyped(const void *message, TickType_t delay = portMAX_DELAY) {
    xQueueSend(queue_, message, delay);
  }
  void ReceiveUntyped(void *message, TickType_t delay = portMAX_DELAY) {
    xQueueReceive(queue_, message, delay);
  }

 private:
  QueueHandle_t queue_;
};

template <typename T>
class RTOSTypedQueue : public RTOSQueueBase {
 public:
  static_assert(
      std::is_trivial_v<T> && std::is_standard_layout_v<T>,
      "T must be a POD type");
  RTOSTypedQueue(QueueHandle_t queue) : RTOSQueueBase(queue) {}

  void Send(const T &message) { RTOSQueueBase::SendUntyped(&message); }
  T Receive() {
    T message;
    RTOSQueueBase::ReceiveUntyped(&message);
    return message;
  }
};

template <typename T>
class RTOSDynamicQueue : public RTOSTypedQueue<T> {
 public:
  RTOSDynamicQueue(int size)
      : RTOSTypedQueue<T>(xQueueCreate(size, sizeof(T))) {}
};

template <typename T, int Size>
class RTOSStaticQueue : public RTOSTypedQueue<T> {
 public:
  RTOSStaticQueue()
      : RTOSTypedQueue<T>(xQueueCreateStatic(Size, sizeof(T), buf_, &queue_)) {}

 private:
  StaticQueue_t queue_;
  uint8_t buf_[Size * sizeof(T)];
};

class MqttClientWrapper {
  using ErrCb = void (*)(void *, err_t);
  ErrCb SendErrToQueue() {
    return +[](void *arg, err_t err) {
      reinterpret_cast<RTOSTypedQueue<err_t> *>(arg)->Send(err);
    };
  }

 public:
  MqttClientWrapper() = delete;

  using TopicFn =
      std::move_only_function<void(std::string_view topic, int num_messages)>;
  using MessageFn = std::move_only_function<void(
      std::span<const uint8_t> message, uint8_t flags)>;

  // Returns an MqttClientWrapper when the client has successfully connected,
  // or else returns an err_t.
  static std::expected<std::unique_ptr<MqttClientWrapper>, err_t> Create(
      std::string_view host, uint16_t port,
      const mqtt_connect_client_info_t &client_info, TopicFn topic_fn,
      MessageFn message_fn) {
    std::unique_ptr<MqttClientWrapper> wrapper(new MqttClientWrapper(
        mqtt_client_new(), std::move(topic_fn), std::move(message_fn)));
    err_t err = wrapper->Connect(host, port, client_info);
    if (err != ERR_OK) {
      return std::unexpected(err);
    }
    return wrapper;
  }

  err_t Publish(
      std::string_view topic, std::string_view message, uint8_t qos,
      uint8_t retain) {
    RTOSStaticQueue<err_t, 1> err_queue;
    err_t err = mqtt_publish(
        client_.get(),
        topic.data(),
        message.data(),
        message.size(),
        qos,
        retain,
        SendErrToQueue(),
        &err_queue);
    err_t cb_err = err_queue.Receive();
    if (err == ERR_OK) err = cb_err;
    if (err != ERR_OK) {
      printf(
          "Error publishing message starting with %s (%d bytes): %s\n",
          std::string(topic.substr(0, 10)).c_str(),
          topic.size(),
          lwip_strerr(err));
    }
    return err;
  }

  err_t Subscribe(std::string_view topic, uint8_t qos) {
    return SubUnsub(topic, qos, true);
  }
  err_t Unsubscribe(std::string_view topic, uint8_t qos) {
    return SubUnsub(topic, qos, false);
  }

  struct ChangeTopic {
    std::string new_topic;
    int num_messages;
  };

 private:
  MqttClientWrapper(
      mqtt_client_t *client, TopicFn topic_fn, MessageFn message_fn)
      : client_(client, mqtt_client_free),
        topic_fn_(std::move(topic_fn)),
        message_fn_(std::move(message_fn)) {}

  err_t Connect(
      std::string_view host, uint16_t port,
      const mqtt_connect_client_info_t &client_info) {
    ip_addr_t address;
    err_t err = netconn_gethostbyname(MQTT_HOST, &address);
    if (err != ERR_OK) {
      printf("Error resolving host: %s, %s\n", MQTT_HOST, lwip_strerr(err));
      return err;
    }

    RTOSStaticQueue<mqtt_connection_status_t, 1> connection_status_queue;
    err = mqtt_client_connect(
        client_.get(),
        &address,
        port,
        +[](mqtt_client_s *, void *arg, mqtt_connection_status_t status) {
          reinterpret_cast<decltype(connection_status_queue) *>(arg)->Send(
              status);
        },
        &connection_status_queue,
        &client_info);
    if (err != ERR_OK) {
      printf("Error connecting to MQTT: %s\n", lwip_strerr(err));
      return err;
    }
    mqtt_connection_status_t connection_status =
        connection_status_queue.Receive();
    if (connection_status != MQTT_CONNECT_ACCEPTED) {
      printf("Connection status != accepted: %d\n", connection_status);
      return ERR_CONN;
    }
    printf("mqtt connected\n");
    mqtt_set_inpub_callback(
        client_.get(),
        +[](void *arg, const char *buf, uint32_t remaining) {
          reinterpret_cast<decltype(this)>(arg)->topic_fn_(
              std::string_view(buf), remaining);
        },
        +[](void *arg, const uint8_t *buf, uint16_t len, uint8_t flags) {
          reinterpret_cast<decltype(this)>(arg)->message_fn_(
              std::span<const uint8_t>(buf, len), flags);
        },
        this);
    return ERR_OK;
  }

  err_t SubUnsub(std::string_view topic, uint8_t qos, bool sub) {
    RTOSStaticQueue<err_t, 1> err_queue;
    err_t err = mqtt_sub_unsub(
        client_.get(), topic.data(), qos, SendErrToQueue(), &err_queue, sub);
    err_t cb_err = err_queue.Receive();
    if (err == ERR_OK) err = cb_err;
    if (err != ERR_OK) {
      printf("Error subscribing to %s: %s\n", topic.data(), lwip_strerr(err));
    }
    printf("%ssubscribed: %s\n", sub ? "" : "un", topic.data());
    return err;
  }

  std::unique_ptr<mqtt_client_t, decltype(&mqtt_client_free)> client_;
  TopicFn topic_fn_;
  MessageFn message_fn_;
};

void mqtt_task(void *) {
  std::expected<std::unique_ptr<MqttClientWrapper>, err_t> mqtt =
      std::unexpected(ERR_CONN);
  while (!mqtt) {
    mqtt = MqttClientWrapper::Create(
        MQTT_HOST,
        MQTT_PORT,
        mqtt_connect_client_info_t{
            .client_id = MQTT_CLIENT_ID,
            .client_user = MQTT_USER,
            .client_pass = MQTT_PASSWORD,
        },
        [&](std::string_view topic, int num_messages) {
          // Do nothing. We only subscribe to one topic in this program,
          // so we don't need to track what we're listening to.
        },
        [&](std::span<const uint8_t> message, uint8_t flags) {
          printf("message received\n");
          if (message.empty()) return;
          CommandMessage m;
          if (message[0] == 'c') {
            m = {.command = Command::Up, .manual = false};
          } else if (message[0] == 'C') {
            // Critical wind speed. Shut immediately.
            m = {.command = Command::Up, .manual = true};
          } else if (message[0] == 'm') {
            m = {.command = Command::My, .manual = false};
          } else {
            return;
          }
          TryPushCommand(m);
        });
    if (!mqtt) vTaskDelay(pdMS_TO_TICKS(5'000));
  }

  mqtt.value()->Subscribe(MQTT_TOPIC, 1);

  while (true) {
    vTaskDelay(portMAX_DELAY);
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

  // Set the emitter pin up.
  gpio_init(15);
  gpio_set_dir(15, true);

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
    driver.SetPower(0b1111'1111);
    driver.SetMode(RfmSpiDriver::kTransmit);
    printf("Waiting for tx ready.\n");
    while (!(driver.GetRegIrqFlags() & RfmSpiDriver::kTxReady)) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    printf("Sending command:%hhu\n", static_cast<byte>(message.command));
    printf("Sending commands disabled during debugging\n");
    remote.sendCommand(message.command, 1);

    driver.SetMode(RfmSpiDriver::kSleep);
    last_message = message;
    last_message_time = receive_time;
    DrainCommandQueue();

    printf("command sent, waiting for next command\n");
  }
}

void create_tasks() {
  xTaskCreate(button_task, "button_task", 512, nullptr, 1, nullptr);
  xTaskCreate(mqtt_task, "mqtt_task", 512, nullptr, 1, nullptr);
  xTaskCreate(radio_task, "radio_task", 512, nullptr, 2, nullptr);
  vTaskDelete(nullptr);
}

}  // namespace
}  // namespace jagsawning

extern "C" void main_task(void *) { jagsawning::create_tasks(); }
#include "homeassistant_service.h"

#include <cassert>
#include <utility>

#include "freertosxx/mutex.h"
#include "freertosxx/queue.h"
#include "lwip/api.h"

namespace jagsawning {

#define MQTTDBG(...) printf(__VA_ARGS__)

using ErrCb = void (*)(void*, err_t);
auto SendErrToQueue() {
  return +[](void* arg, err_t err) {
    reinterpret_cast<freertosxx::Queue<err_t>*>(arg)->Send(err);
  };
}

std::expected<std::unique_ptr<MqttClientWrapper>, err_t>
MqttClientWrapper::Create(
    std::string_view host, uint16_t port,
    const mqtt_connect_client_info_t& client_info) {
  std::unique_ptr<MqttClientWrapper> wrapper(
      new MqttClientWrapper(mqtt_client_new()));
  err_t err = wrapper->Connect(host, port, client_info);
  if (err != ERR_OK) {
    return std::unexpected(err);
  }
  return wrapper;
}

err_t MqttClientWrapper::Publish(
    std::string_view topic, std::string_view message, Qos qos, bool retain) {
  freertosxx::StaticQueue<err_t, 1> err_queue;
  err_t err = mqtt_publish(
      client_.get(),
      topic.data(),
      message.data(),
      message.size(),
      static_cast<uint8_t>(qos),
      retain ? 1 : 0,
      SendErrToQueue(),
      &err_queue);
  if (err != ERR_OK) {
    printf(
        "Error publishing message to %s: %s\n", topic.data(), lwip_strerr(err));
  }
  err_t cb_err = err_queue.Receive();
  if (err == ERR_OK) err = cb_err;
  if (err != ERR_OK) {
    printf(
        "Error publishing message starting with %s (%d bytes): %s\n",
        std::string(topic.substr(0, 10)).c_str(),
        topic.size(),
        lwip_strerr(err));
  } else {
    printf("Published to %s\n", topic.data());
  }
  return err;
}

err_t MqttClientWrapper::Subscribe(
    std::string_view topic_selector, Qos qos, DataHandler handler) {
  {
    freertosxx::MutexLock l(mutex_);
    for (const auto& [key, _] : handlers_) {
      if (key == topic_selector) {
        printf("Error: topic selector already registered: %s\n", key.data());
        return ERR_ARG;
      }
    }
    handlers_.push_back(
        std::make_pair(std::string(topic_selector), std::move(handler)));
  }
  err_t err = SubUnsub(topic_selector, qos, true);
  if (err != ERR_OK) {
    RemoveHandler(topic_selector);
    return err;
  }
  return err;
}

err_t MqttClientWrapper::Unsubscribe(std::string_view topic_selector, Qos qos) {
  {
    freertosxx::MutexLock l(mutex_);
    RemoveHandler(topic_selector);
  }
  return SubUnsub(topic_selector, qos, false);
}

MqttClientWrapper::MqttClientWrapper(mqtt_client_t* client)
    : client_(client, mqtt_client_free) {}
err_t MqttClientWrapper::Connect(
    std::string_view host, uint16_t port,
    const mqtt_connect_client_info_t& client_info) {
  ip_addr_t address;
  err_t err = netconn_gethostbyname(host.data(), &address);
  if (err != ERR_OK) {
    printf("Error resolving host: %s, %s\n", host.data(), lwip_strerr(err));
    return err;
  }

  freertosxx::StaticQueue<mqtt_connection_status_t, 1> connection_status_queue;
  err = mqtt_client_connect(
      client_.get(),
      &address,
      port,
      +[](mqtt_client_s*, void* arg, mqtt_connection_status_t status) {
        reinterpret_cast<decltype(connection_status_queue)*>(arg)->Send(status);
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
      +[](void* arg, const char* buf, uint32_t remaining) {
        reinterpret_cast<decltype(this)>(arg)->ChangeTopic(
            std::string_view(buf), remaining);
      },
      +[](void* arg, const uint8_t* buf, uint16_t len, uint8_t flags) {
        reinterpret_cast<decltype(this)>(arg)->ReceiveMessage(
            std::span<const uint8_t>(buf, len), flags);
      },
      this);
  return ERR_OK;
}

err_t MqttClientWrapper::SubUnsub(std::string_view topic, Qos qos, bool sub) {
  freertosxx::StaticQueue<err_t, 1> err_queue;
  err_t err = mqtt_sub_unsub(
      client_.get(),
      topic.data(),
      static_cast<uint8_t>(qos),
      SendErrToQueue(),
      &err_queue,
      sub);
  err_t cb_err = err_queue.Receive();
  if (err == ERR_OK) err = cb_err;
  if (err != ERR_OK) {
    printf("Error subscribing to %s: %s\n", topic.data(), lwip_strerr(err));
  }
  printf("%ssubscribed: %s\n", sub ? "" : "un", topic.data());
  return err;
}

void MqttClientWrapper::RemoveHandler(std::string_view topic_selector) {
  for (int i = 0; i < handlers_.size(); i++) {
    if (handlers_[i].first == topic_selector) {
      handlers_.erase(handlers_.begin() + i);
      return;
    }
  }
}

bool MqttClientWrapper::TopicMatchesSubscription(
    std::string_view subscription, std::string_view topic) {
  while (!topic.empty()) {
    if (subscription.empty()) return false;
    const std::string_view topic_part = topic.substr(0, topic.find('/'));
    if (subscription.front() == '+') {
      topic.remove_prefix(topic_part.size() + 1);
      subscription.remove_prefix(2);
    } else if (subscription.front() == '#') {
      return true;
    } else {
      const std::string_view subscription_part =
          subscription.substr(0, subscription.find('/'));
      if (topic_part != subscription_part) return false;
      if (topic_part.size() == topic.size() &&
          subscription_part.size() == subscription.size()) {
        return true;
      }
      topic.remove_prefix(topic_part.size() + 1);
      subscription.remove_prefix(subscription_part.size() + 1);
    }
  }
  if (!subscription.empty()) {
    return false;
  }
  return true;
}

void MqttClientWrapper::ChangeTopic(std::string_view topic, int num_messages) {
  MQTTDBG("ChangeTopic(%*s, %d)\n", topic.size(), topic.data(), num_messages);
  freertosxx::MutexLock l(mutex_);
  current_topic_ = topic;
  remaining_messages_ = num_messages;
  if (current_topic_.empty()) {
    assert(remaining_messages_ == 0);
    return;
  }
  for (int i = 0; i < handlers_.size(); ++i) {
    if (TopicMatchesSubscription(handlers_[i].first, current_topic_)) {
      active_handler_ = handlers_[i].second;
      return;
    }
  }
}

void MqttClientWrapper::ReceiveMessage(
    std::span<const uint8_t> message, uint8_t flags) {
  if (active_handler_) {
    Message m{
        current_topic_,
        std::string_view(
            reinterpret_cast<const char*>(message.data()), message.size()),
        flags};
    MQTTDBG(
        "ReceiveMessage(%*s%s, %c)\n",
        std::min<int>(10, m.topic.size()),
        m.topic.data(),
        m.topic.size() > 10 ? "..." : "",
        m.flags);
    active_handler_(std::move(m));
  } else {
    MQTTDBG("ReceiveMessage(): No active handler for message\n");
  }
  if (--remaining_messages_ == 0) {
    MQTTDBG("Topic complete\n");
    active_handler_ = {};
  }
}

#define STATUS_TOPIC "homeassistant/status"
#define COMMAND_TOPIC "homeassistant/cover/awning/set"
#define STATE_TOPIC "homeassistant/cover/awning/state"
#define AVAILABILITY_TOPIC "homeassistant/cover/awning/availability"

#define PAYLOAD_ONLINE "online"
#define PAYLOAD_OFFLINE "offline"

#define PAYLOAD_STATE_OPEN "open"
#define PAYLOAD_STATE_OPENING "opening"
#define PAYLOAD_STATE_CLOSED "closed"
#define PAYLOAD_STATE_CLOSING "closing"
#define PAYLOAD_STATE_STOPPED "stopped"

#define PAYLOAD_CMD_OPEN "OPEN"
#define PAYLOAD_CMD_CLOSE "CLOSE"
#define PAYLOAD_CMD_STOP "STOP"

HomeAssistantService::HomeAssistantService(
    MqttClientWrapper& mqtt_client, SendBlindsCommand send_command)
    : client_(mqtt_client), send_command_(send_command), cover_state_(kClosed) {
  client_.Subscribe(
      STATUS_TOPIC,
      MqttClientWrapper::kAtLeastOnce,
      [&](const MqttClientWrapper::Message& message) {
        ReceiveBirth(message);
      });
  client_.Subscribe(
      COMMAND_TOPIC,
      MqttClientWrapper::kAtLeastOnce,
      [&](const MqttClientWrapper::Message& message) {
        ReceiveCommand(message);
      });
  send_command_(kCmdClose);
  Announce();
}

std::string_view HomeAssistantService::StateToPayload(State s) {
  switch (s) {
    case kOpen:
      return PAYLOAD_STATE_OPEN;
    case kOpening:
      return PAYLOAD_STATE_OPENING;
    case kClosing:
      return PAYLOAD_STATE_CLOSING;
    case kClosed:
      return PAYLOAD_STATE_CLOSED;
    case kStopped:
      return PAYLOAD_STATE_STOPPED;
  }
  __builtin_unreachable();
}

void HomeAssistantService::SetState(State s) {
  cover_state_ = s;
  SendState();
}

void HomeAssistantService::SendState() {
  client_.Publish(
      STATE_TOPIC,
      StateToPayload(cover_state_),
      MqttClientWrapper::kAtLeastOnce,
      false);
}

void HomeAssistantService::ReceiveBirth(
    const MqttClientWrapper::Message& message) {
  if (message.data.starts_with(PAYLOAD_ONLINE)) {
    printf("Received birth message\n");
    Announce();
  } else {
    printf(
        "Received death message? %*s\n",
        message.data.size(),
        message.data.data());
  }
}

void HomeAssistantService::ReceiveCommand(
    const MqttClientWrapper::Message& message) {
  if (message.data.starts_with(PAYLOAD_CMD_OPEN)) {
    send_command_(kCmdOpen);
  } else if (message.data.starts_with(PAYLOAD_CMD_CLOSE)) {
    send_command_(kCmdClose);
  } else if (message.data.starts_with(PAYLOAD_CMD_STOP)) {
    send_command_(kCmdStop);
  } else {
    printf(
        "Received unknown command: %*s\n",
        message.data.size(),
        message.data.data());
  }
}

void HomeAssistantService::Announce() {
  SendDiscovery();
  SendAvailable();
  SendState();
}

void HomeAssistantService::SendAvailable() {
  client_.Publish(
      AVAILABILITY_TOPIC,
      PAYLOAD_ONLINE,
      MqttClientWrapper::kAtLeastOnce,
      false);
}

void HomeAssistantService::SendDiscovery() {
  client_.Publish(
      "homeassistant/cover/awning/config",
      R"({
          "name": "Awning",
          "unique_id": "awning",
          "device_class": "awning",
          "command_topic": ")" COMMAND_TOPIC R"(",
          "state_topic": ")" STATE_TOPIC R"(",
          "availability": {
            "topic": ")" AVAILABILITY_TOPIC R"(",
            "payload_available": ")" PAYLOAD_ONLINE R"(",
            "payload_not_available": ")" PAYLOAD_OFFLINE R"("
          },
          "payload_open": ")" PAYLOAD_CMD_OPEN R"(",
          "payload_close": ")" PAYLOAD_CMD_CLOSE R"(",
          "payload_stop": ")" PAYLOAD_CMD_STOP R"(",
          "state_open": ")" PAYLOAD_STATE_OPEN R"(",
          "state_opening": ")" PAYLOAD_STATE_OPENING R"(",
          "state_closed": ")" PAYLOAD_STATE_CLOSED R"(",
          "state_closing": ")" PAYLOAD_STATE_CLOSING R"(",
          "state_stopped": ")" PAYLOAD_STATE_STOPPED R"(",
          "optimistic": false,
          "retain": true
        })",
      MqttClientWrapper::kAtLeastOnce,
      false);
}

}  // namespace jagsawning
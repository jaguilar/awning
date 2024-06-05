#ifndef HOMEASSISTANT_SERVICE_H
#define HOMEASSISTANT_SERVICE_H

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "freertosxx/mutex.h"
#include "lwip/apps/mqtt.h"
#include "lwip/err.h"

namespace jagsawning {

// Wraps the lwIP MQTT client in a nice (?) C++ interface.
class MqttClientWrapper {
 public:
  enum Qos { kBestEffort = 0, kAtLeastOnce = 1, kAtMostOnce = 2 };

  MqttClientWrapper() = delete;

  // Returns an MqttClientWrapper when the client has successfully connected,
  // or else returns an err_t.
  static std::expected<std::unique_ptr<MqttClientWrapper>, err_t> Create(
      std::string_view host, uint16_t port,
      const mqtt_connect_client_info_t& client_info);

  // Publishes message on topic. qos 0 is ephemeral, 1 is at least once, 2 is at
  // most once.
  err_t Publish(
      std::string_view topic, std::string_view message, Qos qos, bool retain);

  struct Message {
    std::string_view topic;
    std::string_view data;
    uint8_t flags;
  };
  using DataHandler = std::function<void(const Message& message)>;

  // Subscribes to a topic, including topic wildcards. Notifications will be
  // sent to the provided handler function.
  //
  // * It is illegal to add the same topic selector twice.
  // * For a topic published by the broker, the first handler whose selector
  //   matches will be chosen.
  err_t Subscribe(
      std::string_view topic_selector, Qos qos, DataHandler handler);

  // The selector from which to unsubscribe must exactly match the selector
  // previously subscribed to. The handler for topic may be called while
  // Unsubscribe is running but won't be called after Unsubscribe returns.
  //
  // You MUST NOT unsubscribe from a topic in its own handler, or a deadlock
  // may result.
  //
  // Unsubscribing from a selector that that is not subscribed to is a no-op.
  err_t Unsubscribe(std::string_view topic_selector, Qos qos);

 private:
  MqttClientWrapper(mqtt_client_t* client);

  err_t Connect(
      std::string_view host, uint16_t port,
      const mqtt_connect_client_info_t& client_info);

  err_t SubUnsub(std::string_view topic, Qos qos, bool sub);

  void ChangeTopic(std::string_view topic, int num_messages);
  void ReceiveMessage(std::span<const uint8_t> message, uint8_t flags);

  void AddHandler(std::string_view topic_selector, DataHandler handler);
  void RemoveHandler(std::string_view topic_selector);

  bool TopicMatchesSubscription(
      std::string_view subscription, std::string_view topic);

  std::unique_ptr<mqtt_client_t, decltype(&mqtt_client_free)> client_;

  // Only accessed from the callback thread. Thus not guarded by a lock.
  std::string_view current_topic_;
  int remaining_messages_;

  // We check the topic for which handler should handle the message at the
  // beginning of the topic. This way, we can call the handler function without
  // needing to do a map lookup after each message. If a topic is removed from
  // the map,

  freertosxx::Mutex mutex_;  // Guards handlers_ and active_handler_index_.
  std::vector<std::pair<std::string, DataHandler>> handlers_;
  DataHandler active_handler_;
};

// The Somfy Awning home assistant service. Since I have just the one awning,
// this service only announces a single device. It could easily be improved to
// do more, but I won't!
//
// This will announce the device to the home assistant before connecting. If
// there is an error during the announcement phase, an error will be returned
// and the service will be destroyed. Otherwise,
class HomeAssistantService {
 public:
  // open == down closed == up
  enum Command { kCmdOpen = 0b001, kCmdClose = 0b010, kCmdStop = 0b100 };
  using SendBlindsCommand = std::function<void(Command)>;

  static std::expected<std::unique_ptr<HomeAssistantService>, err_t> Connect(
      MqttClientWrapper& mqtt_client, SendBlindsCommand send_command) {
    return std::unique_ptr<HomeAssistantService>{
        new HomeAssistantService(mqtt_client, send_command)};
  }

  enum State {
    kOpen = 0b00001,
    kOpening = 0b00010,
    kClosing = 0b00100,
    kClosed = 0b01000,
    kStopped = 0b10000,
  };
  // Notifies the service that the cover state has changed.
  void SetState(State s);

 private:
  HomeAssistantService(
      MqttClientWrapper& mqtt_client, SendBlindsCommand send_command);

  static std::string_view StateToPayload(State state);

  void Announce();

  void SendDiscovery();
  void SendAvailable();
  void SendState();

  void ReceiveBirth(const MqttClientWrapper::Message& message);
  void ReceiveCommand(const MqttClientWrapper::Message& message);

  MqttClientWrapper& client_;
  SendBlindsCommand send_command_;
  State cover_state_;
};

inline mqtt_connect_client_info_t ConnectInfoFromDefines() {
  return {
      .client_id = MQTT_CLIENT_ID,
      .client_user = MQTT_USER,
      .client_pass = MQTT_PASSWORD,
  };
}

}  // namespace awning

#endif
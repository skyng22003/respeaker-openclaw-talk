#pragma once

#include "audio_convert.h"

#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome::respeaker_realtime {

enum class StopReason : uint8_t {
  LOCAL_STOP = 0,
  MUTED,
  IDLE_TIMEOUT,
  TRANSPORT_ERROR,
  AUDIO_ERROR,
};

class RespeakerRealtime : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_microphone(microphone::Microphone *microphone) { this->microphone_ = microphone; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_bridge_url(const std::string &bridge_url) { this->bridge_url_ = bridge_url; }
  void set_device_id(const std::string &device_id) { this->device_id_ = device_id; }
  void set_credential(const std::string &credential) { this->credential_ = credential; }
  void set_idle_timeout_seconds(uint16_t idle_timeout_seconds) { this->idle_timeout_seconds_ = idle_timeout_seconds; }
  void set_input_channel(uint8_t input_channel) {
    this->input_channel_ = input_channel;
    this->converter_.set_input_channel(input_channel);
  }

  void start_session(const std::string &wake_word);
  void stop_session(StopReason reason);

  uint32_t sent_frames() const { return this->sent_frames_.load(); }
  uint32_t dropped_frames() const { return this->dropped_frames_.load(); }
  uint32_t stale_frames() const { return this->stale_frames_.load(); }
  uint32_t ignored_before_ready() const { return this->ignored_before_ready_.load(); }
  uint32_t reconnect_count() const { return this->reconnect_count_.load(); }
  bool is_transport_ready() const { return this->session_state_.ready(); }

 protected:
  static constexpr size_t UPLINK_QUEUE_DEPTH = 6;
  static constexpr uint8_t MAX_RECONNECT_ATTEMPTS = 5;
  static constexpr uint32_t TRANSPORT_TASK_STACK_WORDS = 6144;

  static void transport_task_entry_(void *parameter);
  void transport_task_();
  static void websocket_event_(void *handler_args, esp_event_base_t event_base, int32_t event_id, void *event_data);
  void handle_websocket_event_(esp_websocket_event_id_t event_id, esp_websocket_event_data_t *event_data);
  void handle_microphone_data_(const std::vector<uint8_t> &data);
  bool open_transport_();
  void close_transport_();
  bool send_hello_();
  bool send_control_(const char *type);
  void handle_text_message_(const char *data, size_t length);
  void clear_uplink_queue_();
  bool pop_uplink_frame_(AudioFrame &frame);
  void fail_session_(const char *safe_code);

  microphone::Microphone *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  std::string bridge_url_;
  std::string device_id_;
  std::string credential_;
  uint16_t idle_timeout_seconds_{30};
  uint8_t input_channel_{0};

  RawCadenceConverter converter_{};
  RawFrameClock raw_clock_{};
  ProductionSessionState session_state_{};
  PcmFrameAssembler assembler_{};
  StaticStaleQueue<AudioFrame, UPLINK_QUEUE_DEPTH> uplink_queue_{};
  portMUX_TYPE queue_mux_ = portMUX_INITIALIZER_UNLOCKED;

  std::atomic<bool> session_requested_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> hello_sent_{false};
  std::atomic<bool> reconnect_needed_{false};
  std::atomic<bool> retry_reset_requested_{false};
  std::atomic<uint32_t> opened_transport_epoch_{0};
  std::atomic<StopReason> stop_reason_{StopReason::LOCAL_STOP};

  std::atomic<uint32_t> sent_frames_{0};
  std::atomic<uint32_t> dropped_frames_{0};
  std::atomic<uint32_t> stale_frames_{0};
  std::atomic<uint32_t> ignored_before_ready_{0};
  std::atomic<uint32_t> reconnect_count_{0};

  esp_websocket_client_handle_t websocket_{nullptr};
  FragmentedTextAssembler text_assembler_{};

  // Accessed only by the microphone callback task.
  uint32_t processed_audio_epoch_{0};
  SessionToken processing_token_{};

  TaskHandle_t transport_task_handle_{nullptr};
  StaticTask_t transport_task_buffer_{};
  std::array<StackType_t, TRANSPORT_TASK_STACK_WORDS> transport_task_stack_{};
};

}  // namespace esphome::respeaker_realtime

#include "respeaker_realtime.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>

namespace esphome::respeaker_realtime {

static const char *const TAG = "respeaker_realtime";

void RespeakerRealtime::setup() {
  if (this->microphone_ == nullptr) {
    ESP_LOGE(TAG, "Microphone is not configured");
    this->mark_failed();
    return;
  }
  if (this->speaker_ == nullptr) {
    ESP_LOGE(TAG, "Speaker is not configured");
    this->mark_failed();
    return;
  }
  const auto stream_info = this->microphone_->get_audio_stream_info();
  if (stream_info.get_sample_rate() != 16000 || stream_info.get_channels() != 2 ||
      stream_info.get_bits_per_sample() != 32) {
    ESP_LOGE(TAG, "Unsupported microphone callback format: %u Hz, %u channels, %u bits",
             static_cast<unsigned>(stream_info.get_sample_rate()), static_cast<unsigned>(stream_info.get_channels()),
             static_cast<unsigned>(stream_info.get_bits_per_sample()));
    this->mark_failed();
    return;
  }

  this->microphone_->add_data_callback(
      [this](const std::vector<uint8_t> &data) { this->handle_microphone_data_(data); });
  this->transport_task_handle_ = xTaskCreateStatic(transport_task_entry_, "realtime_uplink", TRANSPORT_TASK_STACK_WORDS,
                                                   this, 4, this->transport_task_stack_.data(),
                                                   &this->transport_task_buffer_);
  if (this->transport_task_handle_ == nullptr) {
    ESP_LOGE(TAG, "Could not create realtime uplink task");
    this->mark_failed();
  }
}

void RespeakerRealtime::dump_config() {
  ESP_LOGCONFIG(TAG, "reSpeaker realtime uplink:");
  ESP_LOGCONFIG(TAG, "  Transport: %s", this->bridge_url_.rfind("wss://", 0) == 0 ? "wss" : "ws");
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Credential: %s", credential_log_value(this->credential_).c_str());
  ESP_LOGCONFIG(TAG, "  Input channel: %u", static_cast<unsigned>(this->input_channel_));
  ESP_LOGCONFIG(TAG, "  Frame: %u ms / %u bytes", static_cast<unsigned>(PCM_FRAME_MS),
                static_cast<unsigned>(PCM_FRAME_BYTES));
  ESP_LOGCONFIG(TAG, "  Uplink queue depth: %u", static_cast<unsigned>(UPLINK_QUEUE_DEPTH));
  ESP_LOGCONFIG(TAG, "  Idle timeout: %u s", static_cast<unsigned>(this->idle_timeout_seconds_));
}

void RespeakerRealtime::start_session(const std::string &wake_word) {
  (void) wake_word;  // Wake handling and lifecycle integration are added in Task 6.
  bool expected = false;
  if (!this->session_requested_.compare_exchange_strong(expected, true))
    return;

  this->session_state_.start();
  this->connected_.store(false);
  this->hello_sent_.store(false);
  this->reconnect_needed_.store(false);
  this->clear_uplink_queue_();
  this->microphone_->start();
}

void RespeakerRealtime::stop_session(StopReason reason) {
  if (!this->session_requested_.exchange(false))
    return;
  this->stop_reason_.store(reason);
  this->session_state_.lock_send_fence();
  this->session_state_.stop();
  this->session_state_.unlock_send_fence();
  this->hello_sent_.store(false);
  this->clear_uplink_queue_();
  this->microphone_->stop();
}

void RespeakerRealtime::transport_task_entry_(void *parameter) {
  static_cast<RespeakerRealtime *>(parameter)->transport_task_();
}

void RespeakerRealtime::transport_task_() {
  uint8_t attempts = 0;
  TickType_t handshake_started_at = 0;
  bool handshake_pending = false;
  while (true) {
    if (this->retry_reset_requested_.exchange(false))
      attempts = 0;

    if (!this->session_requested_.load()) {
      if (this->websocket_ != nullptr) {
        if (this->connected_.load())
          this->send_control_("stop");
        this->close_transport_();
      }
      attempts = 0;
      handshake_pending = false;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    if (this->websocket_ != nullptr &&
        this->session_state_.needs_reopen(this->opened_transport_epoch_.load())) {
      this->close_transport_();
      this->clear_uplink_queue_();
      handshake_pending = false;
    }

    if (this->reconnect_needed_.exchange(false)) {
      this->close_transport_();
      this->clear_uplink_queue_();
      handshake_pending = false;
      attempts++;
      this->reconnect_count_.fetch_add(1);
    }

    if (this->websocket_ == nullptr) {
      if (attempts >= MAX_RECONNECT_ATTEMPTS) {
        this->fail_session_("transport_retries_exhausted");
        continue;
      }
      if (attempts != 0) {
        const uint32_t shift = std::min<uint8_t>(attempts - 1, 4);
        const uint32_t backoff_ms = std::min<uint32_t>(250U << shift, 4000U);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (!this->session_requested_.load())
          continue;
      }
      if (!this->open_transport_()) {
        this->reconnect_needed_.store(false);
        attempts++;
        this->reconnect_count_.fetch_add(1);
      } else {
        this->opened_transport_epoch_.store(this->session_state_.transport_epoch());
        handshake_started_at = xTaskGetTickCount();
        handshake_pending = true;
      }
      continue;
    }

    if (!this->session_state_.ready()) {
      if (handshake_pending && xTaskGetTickCount() - handshake_started_at >= pdMS_TO_TICKS(5000)) {
        this->reconnect_needed_.store(true);
        handshake_pending = false;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    handshake_pending = false;

    AudioFrame frame;
    if (!this->pop_uplink_frame_(frame)) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    const SessionToken send_token = this->session_state_.capture_send_token();
    if (!this->session_requested_.load() || !this->session_state_.may_send(send_token) || this->websocket_ == nullptr) {
      this->stale_frames_.fetch_add(1);
      continue;
    }
    int written = -1;
    if (!this->session_state_.run_if_current(send_token, [&]() {
          written = esp_websocket_client_send_bin(this->websocket_,
                                                   reinterpret_cast<const char *>(frame.bytes.data()),
                                                   frame.bytes.size(), pdMS_TO_TICKS(40));
        })) {
      this->stale_frames_.fetch_add(1);
      continue;
    }
    if (written == static_cast<int>(frame.bytes.size())) {
      this->sent_frames_.fetch_add(1);
    } else {
      this->dropped_frames_.fetch_add(1);
      this->session_state_.advance_audio_epoch();
      this->reconnect_needed_.store(true);
    }
  }
}

bool RespeakerRealtime::open_transport_() {
  esp_websocket_client_config_t config{};
  config.uri = this->bridge_url_.c_str();
  config.user_context = this;
  config.disable_auto_reconnect = true;
  config.network_timeout_ms = 3000;
  config.buffer_size = 1024;
  config.task_name = "realtime_ws";
  config.task_stack = 6144;
  config.ping_interval_sec = 10;
  if (this->bridge_url_.rfind("wss://", 0) == 0)
    config.crt_bundle_attach = esp_crt_bundle_attach;

  this->websocket_ = esp_websocket_client_init(&config);
  if (this->websocket_ == nullptr)
    return false;
  if (esp_websocket_register_events(this->websocket_, WEBSOCKET_EVENT_ANY, websocket_event_, this) != ESP_OK ||
      esp_websocket_client_start(this->websocket_) != ESP_OK) {
    this->close_transport_();
    return false;
  }
  return true;
}

void RespeakerRealtime::close_transport_() {
  this->session_state_.on_disconnected();
  this->hello_sent_.store(false);
  this->connected_.store(false);
  this->text_assembler_.reset();
  if (this->websocket_ == nullptr)
    return;
  esp_websocket_client_stop(this->websocket_);
  esp_websocket_unregister_events(this->websocket_, WEBSOCKET_EVENT_ANY, websocket_event_);
  esp_websocket_client_destroy(this->websocket_);
  this->websocket_ = nullptr;
}

void RespeakerRealtime::websocket_event_(void *handler_args, esp_event_base_t event_base, int32_t event_id,
                                          void *event_data) {
  (void) event_base;
  static_cast<RespeakerRealtime *>(handler_args)
      ->handle_websocket_event_(static_cast<esp_websocket_event_id_t>(event_id),
                                static_cast<esp_websocket_event_data_t *>(event_data));
}

void RespeakerRealtime::handle_websocket_event_(esp_websocket_event_id_t event_id,
                                                 esp_websocket_event_data_t *event_data) {
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      this->connected_.store(true);
      this->reconnect_needed_.store(false);
      if (!this->session_state_.on_open(this->session_state_.current_session_token()) || !this->send_hello_())
        this->reconnect_needed_.store(true);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
      this->session_state_.on_disconnected();
      this->connected_.store(false);
      this->hello_sent_.store(false);
      if (this->session_requested_.load())
        this->reconnect_needed_.store(true);
      break;
    case WEBSOCKET_EVENT_DATA:
      if (event_data == nullptr || event_data->data_len < 0 || event_data->payload_offset < 0)
        break;
      if (this->text_assembler_.append(event_data->op_code, event_data->fin, event_data->payload_offset,
                                       event_data->data_ptr, static_cast<size_t>(event_data->data_len)) ==
          FragmentedTextAssembler::Result::COMPLETE)
        this->handle_text_message_(this->text_assembler_.data(), this->text_assembler_.size());
      break;
    default:
      break;
  }
}

bool RespeakerRealtime::send_hello_() {
  if (!this->session_requested_.load() || !this->connected_.load() || this->websocket_ == nullptr)
    return false;
  std::array<char, 1024> hello{};
  const int length = std::snprintf(
      hello.data(), hello.size(),
      "{\"type\":\"hello\",\"version\":1,\"generation\":%u,\"deviceId\":\"%s\",\"credential\":\"%s\","
      "\"sampleRate\":24000,\"channels\":1,\"sampleFormat\":\"s16le\",\"frameMs\":20,"
      "\"idleTimeoutSeconds\":%u,\"firmware\":\"respeaker-realtime-task3\"}",
      this->session_state_.generation(), this->device_id_.c_str(), this->credential_.c_str(),
      static_cast<unsigned>(this->idle_timeout_seconds_));
  if (length <= 0 || static_cast<size_t>(length) >= hello.size())
    return false;
  const int written = esp_websocket_client_send_text(this->websocket_, hello.data(), length, pdMS_TO_TICKS(100));
  if (written != length)
    return false;
  this->hello_sent_.store(true);
  this->session_state_.on_hello(this->session_state_.current_session_token());
  return true;
}

bool RespeakerRealtime::send_control_(const char *type) {
  if (!this->connected_.load() || this->websocket_ == nullptr)
    return false;
  std::array<char, 128> message{};
  const int length = std::snprintf(message.data(), message.size(),
                                   "{\"type\":\"%s\",\"version\":1,\"generation\":%u}", type,
                                   this->session_state_.generation());
  if (length <= 0 || static_cast<size_t>(length) >= message.size())
    return false;
  return esp_websocket_client_send_text(this->websocket_, message.data(), length, pdMS_TO_TICKS(100)) == length;
}

void RespeakerRealtime::handle_text_message_(const char *data, size_t length) {
  cJSON *root = cJSON_ParseWithLength(data, length);
  if (root == nullptr)
    return;
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
  const cJSON *generation = cJSON_GetObjectItemCaseSensitive(root, "generation");
  uint32_t message_generation = 0;
  const bool valid_generation =
      cJSON_IsNumber(generation) && generation->valuedouble >= 0 && generation->valuedouble <= UINT32_MAX &&
      (message_generation = static_cast<uint32_t>(generation->valuedouble),
       static_cast<double>(message_generation) == generation->valuedouble);
  const bool current = cJSON_IsString(type) && cJSON_IsNumber(version) && version->valuedouble == 1.0 &&
                       valid_generation && message_generation == this->session_state_.generation();
  if (current && std::strcmp(type->valuestring, "ready") == 0 && this->connected_.load() &&
      this->hello_sent_.load() && this->session_requested_.load()) {
    this->clear_uplink_queue_();
    if (!this->session_state_.on_ready(this->session_state_.current_session_token())) {
      cJSON_Delete(root);
      return;
    }
    this->retry_reset_requested_.store(true);
  } else if (current && std::strcmp(type->valuestring, "ping") == 0) {
    this->send_control_("pong");
  } else if (current && (std::strcmp(type->valuestring, "error") == 0 ||
                         std::strcmp(type->valuestring, "close") == 0)) {
    this->fail_session_("bridge_closed");
  }
  cJSON_Delete(root);
}

void RespeakerRealtime::handle_microphone_data_(const std::vector<uint8_t> &data) {
  const SessionToken callback_token = this->session_state_.capture_audio_token();
  if (!callback_token.valid) {
    this->ignored_before_ready_.fetch_add(1);
    return;
  }

  if (callback_token.audio_epoch != this->processed_audio_epoch_) {
    this->converter_.reset();
    this->raw_clock_.reset();
    this->assembler_.clear();
    this->processed_audio_epoch_ = callback_token.audio_epoch;
  }
  this->processing_token_ = callback_token;

  // The fork discards each raw-read remainder, so callback byte count alone
  // cannot recover capture duration. Carry microsecond time at the raw 48 kHz
  // rate; the first known full callback represents its 256-frame I2S read.
  const int64_t now_us = esp_timer_get_time();
  size_t raw_frames_elapsed = this->raw_clock_.advance(static_cast<uint64_t>(now_us));
  if (raw_frames_elapsed == 0)
    raw_frames_elapsed = 256;
  raw_frames_elapsed = std::min<size_t>(raw_frames_elapsed, 512);

  std::array<int16_t, 256> converted{};
  const size_t sample_count = this->converter_.convert(data.data(), data.size(), raw_frames_elapsed, converted.data(),
                                                       converted.size());
  this->assembler_.append(converted.data(), sample_count, [this](const AudioFrame &audio_frame) {
    if (!this->session_state_.may_emit(this->processing_token_)) {
      this->stale_frames_.fetch_add(1);
      return;
    }
    portENTER_CRITICAL(&this->queue_mux_);
    const bool dropped = this->uplink_queue_.push(audio_frame);
    portEXIT_CRITICAL(&this->queue_mux_);
    if (dropped)
      this->dropped_frames_.fetch_add(1);
  });
}

bool RespeakerRealtime::pop_uplink_frame_(AudioFrame &frame) {
  portENTER_CRITICAL(&this->queue_mux_);
  const bool popped = this->uplink_queue_.pop(frame);
  portEXIT_CRITICAL(&this->queue_mux_);
  return popped;
}

void RespeakerRealtime::clear_uplink_queue_() {
  portENTER_CRITICAL(&this->queue_mux_);
  const size_t stale = this->uplink_queue_.size();
  this->uplink_queue_.clear();
  portEXIT_CRITICAL(&this->queue_mux_);
  this->stale_frames_.fetch_add(stale);
}

void RespeakerRealtime::fail_session_(const char *safe_code) {
  ESP_LOGW(TAG, "Realtime session ended: %s", safe_code);
  this->session_state_.stop();
  this->session_requested_.store(false);
  this->clear_uplink_queue_();
  this->microphone_->stop();
}

}  // namespace esphome::respeaker_realtime

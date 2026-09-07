#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace esphome::respeaker_realtime {

static constexpr size_t INPUT_CHANNELS = 2;
static constexpr size_t INPUT_SAMPLE_BYTES = sizeof(int32_t);
static constexpr size_t INPUT_FRAME_BYTES = INPUT_CHANNELS * INPUT_SAMPLE_BYTES;
static constexpr size_t PCM_SAMPLES_PER_FRAME = 480;
static constexpr size_t PCM_FRAME_BYTES = PCM_SAMPLES_PER_FRAME * sizeof(int16_t);
static constexpr uint32_t PCM_FRAME_MS = 20;

inline int32_t read_s32le(const uint8_t *input) {
  const uint32_t value = static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 8U) |
                         (static_cast<uint32_t>(input[2]) << 16U) | (static_cast<uint32_t>(input[3]) << 24U);
  return static_cast<int32_t>(value);
}

// XVF3800 samples occupy the full signed 32-bit I2S slot. This is an explicit,
// portable arithmetic right shift followed by saturation to signed PCM16.
inline int16_t s32_to_s16_saturated(int32_t sample) {
  const int64_t shifted = sample >= 0 ? static_cast<int64_t>(sample) >> 16U
                                      : -(((-static_cast<int64_t>(sample)) + 0xFFFF) >> 16U);
  if (shifted > INT16_MAX)
    return INT16_MAX;
  if (shifted < INT16_MIN)
    return INT16_MIN;
  return static_cast<int16_t>(shifted);
}

inline size_t convert_48k_stereo_s32_to_24k_mono_s16(const uint8_t *input, size_t input_bytes, int16_t *output,
                                                      size_t output_capacity, uint8_t input_channel = 0) {
  if (input == nullptr || output == nullptr || input_channel >= INPUT_CHANNELS)
    return 0;

  const size_t input_frames = input_bytes / INPUT_FRAME_BYTES;
  size_t output_samples = 0;
  for (size_t frame = 0; frame < input_frames && output_samples < output_capacity; frame += 2) {
    const uint8_t *sample = input + frame * INPUT_FRAME_BYTES + input_channel * INPUT_SAMPLE_BYTES;
    output[output_samples++] = s32_to_s16_saturated(read_s32le(sample));
  }
  return output_samples;
}

inline size_t convert_48k_stereo_s32le_to_24k_mono_s16le(const uint8_t *input, size_t input_bytes, int16_t *output,
                                                          size_t output_capacity) {
  return convert_48k_stereo_s32_to_24k_mono_s16(input, input_bytes, output, output_capacity, 0);
}

class AudioStreamConverter {
 public:
  explicit AudioStreamConverter(uint8_t input_channel = 0) : input_channel_(input_channel > 1 ? 0 : input_channel) {}

  void reset() { this->phase_ = 0; }
  void set_input_channel(uint8_t input_channel) { this->input_channel_ = input_channel > 1 ? 0 : input_channel; }
  uint8_t phase() const { return this->phase_; }

  bool convert_frame(const uint8_t *stereo_frame, int16_t &output) {
    const bool emit = this->phase_ == 0;
    this->phase_ ^= 1U;
    if (!emit)
      return false;
    output = s32_to_s16_saturated(read_s32le(stereo_frame + this->input_channel_ * INPUT_SAMPLE_BYTES));
    return true;
  }

  size_t convert(const uint8_t *input, size_t input_bytes, int16_t *output, size_t output_capacity) {
    if (input == nullptr || output == nullptr)
      return 0;
    const size_t input_frames = input_bytes / INPUT_FRAME_BYTES;
    size_t output_samples = 0;
    for (size_t frame = 0; frame < input_frames && output_samples < output_capacity; frame++) {
      int16_t sample = 0;
      if (this->convert_frame(input + frame * INPUT_FRAME_BYTES, sample))
        output[output_samples++] = sample;
    }
    return output_samples;
  }

 private:
  uint8_t input_channel_{0};
  uint8_t phase_{0};
};

// The repository's pinned `respeaker_microphone` ESPHome fork reads the
// hardware at 48 kHz but retains every third 8-byte stereo frame before it
// invokes microphone callbacks. The callback contract consumed by this
// component is consequently 16 kHz stereo S32LE, not the raw 48 kHz bus.
// Resample that stream to the bridge's negotiated 24 kHz mono PCM16 rate.
class MicrophoneCallbackConverter {
 public:
  explicit MicrophoneCallbackConverter(uint8_t input_channel = 0)
      : input_channel_(input_channel > 1 ? 0 : input_channel) {}

  void reset() {
    this->have_previous_ = false;
    this->two_outputs_this_interval_ = false;
  }
  void set_input_channel(uint8_t input_channel) { this->input_channel_ = input_channel > 1 ? 0 : input_channel; }

  size_t convert_frame(const uint8_t *stereo_frame, int16_t output[2]) {
    const int16_t current =
        s32_to_s16_saturated(read_s32le(stereo_frame + this->input_channel_ * INPUT_SAMPLE_BYTES));
    if (!this->have_previous_) {
      output[0] = current;
      this->previous_ = current;
      this->have_previous_ = true;
      return 1;
    }

    size_t count = 1;
    if (this->two_outputs_this_interval_) {
      output[0] = this->interpolate_thirds_(this->previous_, current, 1);
      output[1] = current;
      count = 2;
    } else {
      output[0] = this->interpolate_thirds_(this->previous_, current, 2);
    }
    this->two_outputs_this_interval_ = !this->two_outputs_this_interval_;
    this->previous_ = current;
    return count;
  }

 private:
  static int16_t interpolate_thirds_(int16_t from, int16_t to, int numerator) {
    return static_cast<int16_t>((static_cast<int32_t>(from) * (3 - numerator) +
                                 static_cast<int32_t>(to) * numerator) /
                                3);
  }

  uint8_t input_channel_{0};
  bool have_previous_{false};
  bool two_outputs_this_interval_{false};
  int16_t previous_{0};
};

struct AudioFrame {
  std::array<uint8_t, PCM_FRAME_BYTES> bytes{};
};

class PcmFrameAssembler {
 public:
  void clear() { this->sample_count_ = 0; }
  size_t pending_samples() const { return this->sample_count_; }

  template<typename Emit> void append(const int16_t *samples, size_t count, Emit &&emit) {
    for (size_t i = 0; i < count; i++) {
      const uint16_t value = static_cast<uint16_t>(samples[i]);
      const size_t offset = this->sample_count_ * sizeof(int16_t);
      this->frame_.bytes[offset] = static_cast<uint8_t>(value);
      this->frame_.bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
      this->sample_count_++;
      if (this->sample_count_ == PCM_SAMPLES_PER_FRAME) {
        emit(this->frame_);
        this->sample_count_ = 0;
      }
    }
  }

 private:
  AudioFrame frame_{};
  size_t sample_count_{0};
};

template<typename T, size_t Capacity> class StaticStaleQueue {
  static_assert(Capacity > 0, "Queue capacity must be positive");

 public:
  bool push(const T &value) {
    bool dropped = false;
    if (this->size_ == Capacity) {
      this->head_ = (this->head_ + 1) % Capacity;
      this->size_--;
      this->dropped_++;
      dropped = true;
    }
    const size_t tail = (this->head_ + this->size_) % Capacity;
    this->values_[tail] = value;
    this->size_++;
    return dropped;
  }

  bool pop(T &value) {
    if (this->size_ == 0)
      return false;
    value = this->values_[this->head_];
    this->head_ = (this->head_ + 1) % Capacity;
    this->size_--;
    return true;
  }

  void clear() {
    this->stale_cleared_ += this->size_;
    this->head_ = 0;
    this->size_ = 0;
  }

  size_t size() const { return this->size_; }
  uint32_t dropped() const { return this->dropped_; }
  uint32_t stale_cleared() const { return this->stale_cleared_; }

 private:
  std::array<T, Capacity> values_{};
  size_t head_{0};
  size_t size_{0};
  uint32_t dropped_{0};
  uint32_t stale_cleared_{0};
};

class SessionLifecycle {
 public:
  bool start(uint32_t generation) {
    if (this->active_)
      return false;
    this->active_ = true;
    this->connected_ = false;
    this->hello_sent_ = false;
    this->ready_ = false;
    this->generation_ = generation;
    this->backoff_ms_ = 250;
    return true;
  }

  bool on_connected() {
    if (!this->active_ || this->connected_)
      return false;
    this->connected_ = true;
    this->hello_sent_ = false;
    this->ready_ = false;
    return true;
  }

  void on_hello_sent() {
    if (this->connected_)
      this->hello_sent_ = true;
  }

  bool on_ready(uint32_t generation) {
    if (!this->active_ || !this->connected_ || !this->hello_sent_ || generation != this->generation_)
      return false;
    this->ready_ = true;
    this->backoff_ms_ = 250;
    return true;
  }

  void on_disconnected() {
    this->connected_ = false;
    this->hello_sent_ = false;
    this->ready_ = false;
  }

  uint32_t next_backoff_ms() {
    const uint32_t value = this->backoff_ms_;
    if (this->backoff_ms_ < 4000)
      this->backoff_ms_ *= 2;
    return value;
  }

  void stop() {
    this->active_ = false;
    this->on_disconnected();
  }

  bool active() const { return this->active_; }
  bool connected() const { return this->connected_; }
  bool hello_sent() const { return this->hello_sent_; }
  bool can_send_audio() const { return this->active_ && this->connected_ && this->hello_sent_ && this->ready_; }
  uint32_t generation() const { return this->generation_; }

 private:
  bool active_{false};
  bool connected_{false};
  bool hello_sent_{false};
  bool ready_{false};
  uint32_t generation_{0};
  uint32_t backoff_ms_{250};
};

inline std::string credential_log_value(const std::string &) { return "<redacted>"; }

}  // namespace esphome::respeaker_realtime

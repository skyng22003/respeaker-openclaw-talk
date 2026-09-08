#pragma once

#include <array>
#include <atomic>
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

// The pinned microphone fork restarts its every-third selector for each raw
// read and discards the read remainder. `raw_frames_elapsed` restores that
// missing time contract. The converter emits one sample per two raw 48 kHz
// frames and carries that phase across callbacks. Samples in selector gaps use
// the most recent value; this keeps cadence exact without allocation.
class RawCadenceConverter {
 public:
  explicit RawCadenceConverter(uint8_t input_channel = 0) : input_channel_(input_channel > 1 ? 0 : input_channel) {}

  void reset() {
    this->raw_phase_ = 0;
    this->last_sample_ = 0;
  }
  void set_input_channel(uint8_t input_channel) { this->input_channel_ = input_channel > 1 ? 0 : input_channel; }
  uint8_t raw_phase() const { return this->raw_phase_; }

  size_t convert(const uint8_t *selected_frames, size_t selected_bytes, size_t raw_frames_elapsed, int16_t *output,
                 size_t output_capacity) {
    if (selected_frames == nullptr || output == nullptr)
      return 0;
    const size_t selected_count = selected_bytes / INPUT_FRAME_BYTES;
    size_t selected_index = 0;
    size_t output_count = 0;
    for (size_t raw = 0; raw < raw_frames_elapsed; raw++) {
      if (raw % 3 == 0 && selected_index < selected_count) {
        this->last_sample_ = s32_to_s16_saturated(
            read_s32le(selected_frames + selected_index * INPUT_FRAME_BYTES + this->input_channel_ * INPUT_SAMPLE_BYTES));
        selected_index++;
      }
      if (this->raw_phase_ == 0 && output_count < output_capacity)
        output[output_count++] = this->last_sample_;
      this->raw_phase_ ^= 1U;
    }
    return output_count;
  }

 private:
  uint8_t input_channel_{0};
  uint8_t raw_phase_{0};
  int16_t last_sample_{0};
};

class RawFrameClock {
 public:
  size_t advance(uint64_t now_us) {
    if (!this->started_) {
      this->started_ = true;
      this->last_us_ = now_us;
      return 0;
    }
    if (now_us <= this->last_us_)
      return 0;
    const uint64_t scaled = (now_us - this->last_us_) * 48000ULL + this->remainder_;
    this->last_us_ = now_us;
    this->remainder_ = scaled % 1000000ULL;
    return scaled / 1000000ULL;
  }

  void reset() {
    this->started_ = false;
    this->last_us_ = 0;
    this->remainder_ = 0;
  }

 private:
  bool started_{false};
  uint64_t last_us_{0};
  uint64_t remainder_{0};
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

struct SessionToken {
  uint32_t generation{0};
  uint32_t transport_epoch{0};
  uint32_t audio_epoch{0};
  bool valid{false};
};

// This is the production lifecycle fence used by RespeakerRealtime. Tokens
// make every asynchronous transport send and callback emission prove that it
// still belongs to the active session, connection, and readiness epoch.
class ProductionSessionState {
 public:
  SessionToken start() {
    this->active_.store(true);
    this->connected_.store(false);
    this->hello_sent_.store(false);
    this->ready_.store(false);
    this->generation_.fetch_add(1);
    this->transport_epoch_.fetch_add(1);
    this->audio_epoch_.fetch_add(1);
    return this->snapshot_();
  }

  void stop() {
    this->active_.store(false);
    this->connected_.store(false);
    this->hello_sent_.store(false);
    this->ready_.store(false);
    this->transport_epoch_.fetch_add(1);
    this->audio_epoch_.fetch_add(1);
  }

  bool needs_reopen(uint32_t opened_epoch) const {
    return this->active_.load() && opened_epoch != this->transport_epoch_.load();
  }

  bool on_open(const SessionToken &token) {
    if (!this->matches_session_(token))
      return false;
    this->connected_.store(true);
    this->hello_sent_.store(false);
    this->ready_.store(false);
    return true;
  }

  bool on_hello(const SessionToken &token) {
    if (!this->matches_session_(token) || !this->connected_.load())
      return false;
    this->hello_sent_.store(true);
    return true;
  }

  bool on_ready(const SessionToken &token) {
    if (!this->matches_session_(token) || !this->connected_.load() || !this->hello_sent_.load() ||
        token.audio_epoch != this->audio_epoch_.load())
      return false;
    this->audio_epoch_.fetch_add(1);
    this->ready_.store(true);
    return true;
  }

  void on_disconnected() {
    this->connected_.store(false);
    this->hello_sent_.store(false);
    this->ready_.store(false);
    this->audio_epoch_.fetch_add(1);
  }

  SessionToken capture_send_token() const {
    SessionToken token = this->snapshot_();
    token.valid = token.valid && this->connected_.load() && this->hello_sent_.load() && this->ready_.load();
    return token;
  }

  SessionToken capture_audio_token() const { return this->capture_send_token(); }
  SessionToken current_session_token() const { return this->snapshot_(); }

  bool may_send(const SessionToken &token) const {
    return token.valid && this->matches_session_(token) && token.audio_epoch == this->audio_epoch_.load() &&
           this->connected_.load() && this->hello_sent_.load() && this->ready_.load();
  }

  bool may_emit(const SessionToken &token) const { return this->may_send(token); }
  template<typename Send> bool run_if_current(const SessionToken &token, Send &&send) {
    while (this->send_lock_.test_and_set(std::memory_order_acquire)) {
    }
    if (!this->may_send(token)) {
      this->send_lock_.clear(std::memory_order_release);
      return false;
    }
    send();
    this->send_lock_.clear(std::memory_order_release);
    return true;
  }
  void lock_send_fence() {
    while (this->send_lock_.test_and_set(std::memory_order_acquire)) {
    }
  }
  void unlock_send_fence() { this->send_lock_.clear(std::memory_order_release); }
  void advance_audio_epoch() {
    this->ready_.store(false);
    this->audio_epoch_.fetch_add(1);
  }
  bool active() const { return this->active_.load(); }
  bool ready() const { return this->ready_.load(); }
  uint32_t generation() const { return this->generation_.load(); }
  uint32_t transport_epoch() const { return this->transport_epoch_.load(); }
  uint32_t audio_epoch() const { return this->audio_epoch_.load(); }

 private:
  SessionToken snapshot_() const {
    return {this->generation_.load(), this->transport_epoch_.load(), this->audio_epoch_.load(), this->active_.load()};
  }
  bool matches_session_(const SessionToken &token) const {
    return token.valid && this->active_.load() && token.generation == this->generation_.load() &&
           token.transport_epoch == this->transport_epoch_.load();
  }

  std::atomic<bool> active_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> hello_sent_{false};
  std::atomic<bool> ready_{false};
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> transport_epoch_{0};
  std::atomic<uint32_t> audio_epoch_{0};
  std::atomic_flag send_lock_ = ATOMIC_FLAG_INIT;
};

class FragmentedTextAssembler {
 public:
  enum class Result : uint8_t { REJECTED = 0, INCOMPLETE, COMPLETE };

  Result append(uint8_t opcode, bool fin, size_t payload_offset, const char *data, size_t length) {
    if (opcode == 0x01 && payload_offset == 0) {
      this->length_ = 0;
      this->frame_base_ = 0;
      this->in_progress_ = true;
    } else if (opcode == 0x00 && payload_offset == 0 && this->in_progress_) {
      this->frame_base_ = this->length_;
    } else if (opcode != 0x01 && opcode != 0x00) {
      return Result::REJECTED;
    }
    if (!this->in_progress_ || this->frame_base_ + payload_offset != this->length_ ||
        this->length_ + length >= this->message_.size()) {
      this->reset();
      return Result::REJECTED;
    }
    std::memcpy(this->message_.data() + this->length_, data, length);
    this->length_ += length;
    if (!fin)
      return Result::INCOMPLETE;
    this->message_[this->length_] = '\0';
    this->in_progress_ = false;
    return Result::COMPLETE;
  }

  void reset() {
    this->length_ = 0;
    this->frame_base_ = 0;
    this->in_progress_ = false;
  }
  const char *data() const { return this->message_.data(); }
  size_t size() const { return this->length_; }

 private:
  std::array<char, 512> message_{};
  size_t length_{0};
  size_t frame_base_{0};
  bool in_progress_{false};
};

inline std::string credential_log_value(const std::string &) { return "<redacted>"; }

}  // namespace esphome::respeaker_realtime

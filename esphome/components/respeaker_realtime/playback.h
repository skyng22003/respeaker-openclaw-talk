#pragma once

#include "audio_convert.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::respeaker_realtime {

// Queued playback audio carries the session ownership it was accepted under and
// the clear epoch that was current at acceptance. Anything whose ownership no
// longer matches is discarded instead of played, so audio from a superseded
// session, a closed transport, or a cleared turn can never reach the speaker.
struct PlaybackFrame {
  AudioFrame audio{};
  SessionToken token{};
  uint32_t clear_epoch{0};
};

// Host tests drive the engine from a single thread; production supplies a
// spinlock wrapper. The lock only ever guards the bounded queue, never a
// speaker call.
struct NoOpPlaybackLock {
  void lock() {}
  void unlock() {}
};

// PlaybackEngine owns the downlink jitter buffer and is the only place that
// talks to the speaker. `submit` is called from the WebSocket event callback
// and must never block: it validates, copies, and returns. `pump` is called
// exclusively from the dedicated playback task, which is the only context
// allowed to call start/play/stop on the speaker.
//
// Termination thresholds: MAX_CONSECUTIVE_STALLS consecutive pump calls where
// the speaker accepts zero bytes (1 s at the 20 ms play timeout), and
// MAX_CONSECUTIVE_OVERFLOWS consecutive submits that had to drop a stale frame,
// both raise a latched audio fault for the owning task to act on.
template<typename SpeakerT, size_t Capacity, typename LockT = NoOpPlaybackLock> class PlaybackEngine {
  static_assert(Capacity > 0, "Playback queue capacity must be positive");

 public:
  enum class Submit : uint8_t { ACCEPTED = 0, REJECTED_INVALID, REJECTED_NOT_READY };

  static constexpr uint32_t MAX_CONSECUTIVE_STALLS = 50;
  static constexpr uint32_t MAX_CONSECUTIVE_OVERFLOWS = 25;

  void attach_speaker(SpeakerT *speaker) { this->speaker_ = speaker; }

  Submit submit(const uint8_t *data, size_t length, const ProductionSessionState &state, const SessionToken &token) {
    if (data == nullptr || length != PCM_FRAME_BYTES) {
      this->invalid_frames_.fetch_add(1);
      return Submit::REJECTED_INVALID;
    }
    if (!state.may_emit(token)) {
      this->rejected_before_ready_.fetch_add(1);
      return Submit::REJECTED_NOT_READY;
    }

    PlaybackFrame frame{};
    std::memcpy(frame.audio.bytes.data(), data, PCM_FRAME_BYTES);
    frame.token = token;
    frame.clear_epoch = this->clear_epoch_.load();

    this->lock_.lock();
    const bool dropped = this->queue_.push(frame);
    this->lock_.unlock();

    if (dropped) {
      this->overflow_frames_.fetch_add(1);
      if (this->consecutive_overflows_.fetch_add(1) + 1 >= MAX_CONSECUTIVE_OVERFLOWS)
        this->audio_fault_.store(true);
    } else {
      this->consecutive_overflows_.store(0);
    }
    return Submit::ACCEPTED;
  }

  // Invalidates queued and in-flight audio and asks the playback task to stop
  // the speaker. Safe from any task: it never touches the speaker itself.
  void request_clear() {
    this->clear_epoch_.fetch_add(1);
    this->lock_.lock();
    const size_t cleared = this->queue_.size();
    this->queue_.clear();
    this->lock_.unlock();
    this->discarded_frames_.fetch_add(static_cast<uint32_t>(cleared));
    this->stop_requested_.store(true);
  }

  // Runs only on the dedicated playback task. Returns true when it did work and
  // false when the queue was empty, so the caller can idle.
  bool pump(const ProductionSessionState &state, uint32_t play_timeout) {
    if (this->stop_requested_.exchange(false)) {
      this->discard_in_flight_();
      this->stop_speaker_();
    }

    if (this->in_flight_ && !this->frame_is_current_(state, this->current_)) {
      this->discard_in_flight_();
      this->stop_speaker_();
    }

    if (!this->in_flight_) {
      PlaybackFrame next{};
      this->lock_.lock();
      const bool popped = this->queue_.pop(next);
      this->lock_.unlock();
      if (!popped) {
        this->underflow_polls_.fetch_add(1);
        return false;
      }
      if (!this->frame_is_current_(state, next)) {
        this->discarded_frames_.fetch_add(1);
        return true;
      }
      this->current_ = next;
      this->offset_ = 0;
      this->in_flight_ = true;
    }

    if (this->speaker_ == nullptr) {
      this->discard_in_flight_();
      return true;
    }
    if (!this->started_) {
      this->speaker_->start();
      this->started_ = true;
    }

    const size_t remaining = PCM_FRAME_BYTES - this->offset_;
    size_t written = this->speaker_->play(this->current_.audio.bytes.data() + this->offset_, remaining, play_timeout);
    if (written > remaining)
      written = remaining;
    this->offset_ += written;

    if (this->offset_ == PCM_FRAME_BYTES) {
      this->played_frames_.fetch_add(1);
      this->in_flight_ = false;
      this->offset_ = 0;
      this->consecutive_stalls_.store(0);
      return true;
    }

    // Only the unwritten suffix of the current frame is retained; the accepted
    // prefix is never replayed.
    if (written == 0) {
      if (this->consecutive_stalls_.fetch_add(1) + 1 >= MAX_CONSECUTIVE_STALLS)
        this->audio_fault_.store(true);
    } else {
      this->partial_writes_.fetch_add(1);
      this->consecutive_stalls_.store(0);
    }
    return true;
  }

  // Latched fault consumed by the task that owns session lifecycle.
  bool take_fault() { return this->audio_fault_.exchange(false); }
  bool fault_pending() const { return this->audio_fault_.load(); }

  size_t queued_frames() const { return this->queue_.size(); }
  bool has_in_flight() const { return this->in_flight_; }
  size_t in_flight_offset() const { return this->offset_; }
  bool speaker_started() const { return this->started_; }

  uint32_t played_frames() const { return this->played_frames_.load(); }
  uint32_t overflow_frames() const { return this->overflow_frames_.load(); }
  uint32_t discarded_frames() const { return this->discarded_frames_.load(); }
  uint32_t rejected_before_ready() const { return this->rejected_before_ready_.load(); }
  uint32_t invalid_frames() const { return this->invalid_frames_.load(); }
  uint32_t partial_writes() const { return this->partial_writes_.load(); }
  uint32_t underflow_polls() const { return this->underflow_polls_.load(); }
  uint32_t dropped_frames() const { return this->overflow_frames_.load() + this->discarded_frames_.load(); }

 protected:
  bool frame_is_current_(const ProductionSessionState &state, const PlaybackFrame &frame) const {
    return frame.clear_epoch == this->clear_epoch_.load() && state.may_emit(frame.token);
  }

  void discard_in_flight_() {
    if (!this->in_flight_)
      return;
    this->discarded_frames_.fetch_add(1);
    this->in_flight_ = false;
    this->offset_ = 0;
  }

  void stop_speaker_() {
    if (this->speaker_ == nullptr || !this->started_)
      return;
    this->speaker_->stop();
    this->started_ = false;
  }

  SpeakerT *speaker_{nullptr};
  StaticStaleQueue<PlaybackFrame, Capacity> queue_{};
  LockT lock_{};

  // Touched only by the playback task.
  PlaybackFrame current_{};
  size_t offset_{0};
  bool in_flight_{false};
  bool started_{false};

  std::atomic<uint32_t> clear_epoch_{0};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> audio_fault_{false};

  std::atomic<uint32_t> played_frames_{0};
  std::atomic<uint32_t> overflow_frames_{0};
  std::atomic<uint32_t> discarded_frames_{0};
  std::atomic<uint32_t> rejected_before_ready_{0};
  std::atomic<uint32_t> invalid_frames_{0};
  std::atomic<uint32_t> partial_writes_{0};
  std::atomic<uint32_t> underflow_polls_{0};
  std::atomic<uint32_t> consecutive_stalls_{0};
  std::atomic<uint32_t> consecutive_overflows_{0};
};

}  // namespace esphome::respeaker_realtime

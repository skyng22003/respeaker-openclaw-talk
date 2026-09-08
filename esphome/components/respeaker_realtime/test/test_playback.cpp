#include "../playback.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using esphome::respeaker_realtime::AudioFrame;
using esphome::respeaker_realtime::PCM_FRAME_BYTES;
using esphome::respeaker_realtime::PlaybackEngine;
using esphome::respeaker_realtime::ProductionSessionState;
using esphome::respeaker_realtime::SessionToken;

namespace {

int failures = 0;

#define EXPECT_TRUE(value)                                                                                             \
  do {                                                                                                                 \
    if (!(value)) {                                                                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": expected true: " #value << '\n';                                  \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

#define EXPECT_EQ(actual, expected)                                                                                    \
  do {                                                                                                                 \
    const auto actual_value = (actual);                                                                                \
    const auto expected_value = (expected);                                                                            \
    if (!(actual_value == expected_value)) {                                                                           \
      std::cerr << __FILE__ << ':' << __LINE__ << ": expected " #actual " == " #expected << '\n';                      \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

// Stands in for speaker::Speaker. It records every byte it accepted so tests
// can prove exact ordering and that no accepted prefix is ever replayed.
class FakeSpeaker {
 public:
  void start() {
    this->start_calls_++;
    this->running_ = true;
  }

  void stop() {
    this->stop_calls_++;
    this->running_ = false;
  }

  size_t play(const uint8_t *data, size_t length, uint32_t ticks_to_wait) {
    (void) ticks_to_wait;
    this->play_calls_++;
    const size_t accepted = std::min(length, this->accept_limit_);
    this->received_.insert(this->received_.end(), data, data + accepted);
    return accepted;
  }

  void set_accept_limit(size_t accept_limit) { this->accept_limit_ = accept_limit; }
  const std::vector<uint8_t> &received() const { return this->received_; }
  uint32_t start_calls() const { return this->start_calls_; }
  uint32_t stop_calls() const { return this->stop_calls_; }
  uint32_t play_calls() const { return this->play_calls_; }
  bool running() const { return this->running_; }

 private:
  std::vector<uint8_t> received_{};
  size_t accept_limit_{PCM_FRAME_BYTES};
  uint32_t start_calls_{0};
  uint32_t stop_calls_{0};
  uint32_t play_calls_{0};
  bool running_{false};
};

using Engine = PlaybackEngine<FakeSpeaker, 3>;

SessionToken bring_ready(ProductionSessionState &state) {
  state.start();
  state.on_open(state.current_session_token());
  state.on_hello(state.current_session_token());
  state.on_ready(state.current_session_token());
  return state.capture_audio_token();
}

AudioFrame filled_frame(uint8_t value) {
  AudioFrame frame{};
  frame.bytes.fill(value);
  return frame;
}

void test_frames_are_rejected_before_ready_and_when_malformed() {
  ProductionSessionState state{};
  FakeSpeaker speaker{};
  Engine engine{};
  engine.attach_speaker(&speaker);

  const AudioFrame frame = filled_frame(0x11);

  // No session at all.
  EXPECT_TRUE(engine.submit(frame.bytes.data(), frame.bytes.size(), state, state.capture_audio_token()) ==
              Engine::Submit::REJECTED_NOT_READY);

  // Session started but bridge has not sent ready.
  state.start();
  state.on_open(state.current_session_token());
  state.on_hello(state.current_session_token());
  const SessionToken pre_ready = state.current_session_token();
  EXPECT_TRUE(engine.submit(frame.bytes.data(), frame.bytes.size(), state, pre_ready) ==
              Engine::Submit::REJECTED_NOT_READY);

  const SessionToken token = bring_ready(state);
  EXPECT_TRUE(engine.submit(frame.bytes.data(), PCM_FRAME_BYTES - 1, state, token) == Engine::Submit::REJECTED_INVALID);
  EXPECT_TRUE(engine.submit(frame.bytes.data(), PCM_FRAME_BYTES + 1, state, token) == Engine::Submit::REJECTED_INVALID);
  EXPECT_TRUE(engine.submit(nullptr, PCM_FRAME_BYTES, state, token) == Engine::Submit::REJECTED_INVALID);

  EXPECT_EQ(engine.rejected_before_ready(), 2U);
  EXPECT_EQ(engine.invalid_frames(), 3U);
  EXPECT_EQ(engine.queued_frames(), 0U);
  EXPECT_EQ(speaker.play_calls(), 0U);
}

void test_submit_never_touches_the_speaker() {
  ProductionSessionState state{};
  FakeSpeaker speaker{};
  Engine engine{};
  engine.attach_speaker(&speaker);
  const SessionToken token = bring_ready(state);

  const AudioFrame frame = filled_frame(0x22);
  EXPECT_TRUE(engine.submit(frame.bytes.data(), frame.bytes.size(), state, token) == Engine::Submit::ACCEPTED);

  // The WebSocket callback path only queues; playback is the task's job.
  EXPECT_EQ(speaker.start_calls(), 0U);
  EXPECT_EQ(speaker.play_calls(), 0U);
  EXPECT_EQ(engine.played_frames(), 0U);
  EXPECT_EQ(engine.queued_frames(), 1U);
}

void test_frames_play_in_order_one_per_pump() {
  ProductionSessionState state{};
  FakeSpeaker speaker{};
  Engine engine{};
  engine.attach_speaker(&speaker);
  const SessionToken token = bring_ready(state);

  std::vector<uint8_t> expected;
  for (uint8_t value = 1; value <= 3; value++) {
    const AudioFrame frame = filled_frame(value);
    EXPECT_TRUE(engine.submit(frame.bytes.data(), frame.bytes.size(), state, token) == Engine::Submit::ACCEPTED);
    expected.insert(expected.end(), frame.bytes.begin(), frame.bytes.end());
  }

  for (int i = 0; i < 3; i++)
    EXPECT_TRUE(engine.pump(state, 20));

  EXPECT_EQ(engine.played_frames(), 3U);
  EXPECT_EQ(engine.queued_frames(), 0U);
  EXPECT_EQ(speaker.start_calls(), 1U);
  EXPECT_TRUE(speaker.received() == expected);
  EXPECT_TRUE(!engine.pump(state, 20));  // Empty queue reports underflow.
  EXPECT_EQ(engine.underflow_polls(), 1U);
}

void test_queue_is_bounded_and_drops_stale_first() {
  ProductionSessionState state{};
  FakeSpeaker speaker{};
  Engine engine{};
  engine.attach_speaker(&speaker);
  const SessionToken token = bring_ready(state);

  for (uint8_t value = 1; value <= 5; value++) {
    const AudioFrame frame = filled_frame(value);
    engine.submit(frame.bytes.data(), frame.bytes.size(), state, token);
  }

  EXPECT_EQ(engine.queued_frames(), 3U);
  EXPECT_EQ(engine.overflow_frames(), 2U);

  for (int i = 0; i < 3; i++)
    engine.pump(state, 20);

  // The oldest two frames were dropped; playback resumes at frame 3.
  EXPECT_EQ(speaker.received().size(), 3U * PCM_FRAME_BYTES);
  EXPECT_EQ(speaker.received().front(), static_cast<uint8_t>(3));
  EXPECT_EQ(speaker.received().back(), static_cast<uint8_t>(5));
}

void test_clear_discards_queued_and_in_flight_audio_then_stops() {
  ProductionSessionState state{};
  FakeSpeaker speaker{};
  Engine engine{};
  engine.attach_speaker(&speaker);
  const SessionToken token = bring_ready(state);

  speaker.set_accept_limit(400);
  const AudioFrame first = filled_frame(0x31);
  engine.submit(first.bytes.data(), first.bytes.size(), state, token);
  engine.pump(state, 20);
  EXPECT_TRUE(engine.has_in_flight());
  EXPECT_EQ(engine.in_flight_offset(), 400U);

  const AudioFrame second = filled_frame(0x32);
  engine.submit(second.bytes.data(), second.bytes.size(), state, token);
  EXPECT_EQ(engine.queued_frames(), 1U);

  engine.request_clear();
  EXPECT_EQ(engine.queued_frames(), 0U);

  speaker.set_accept_limit(PCM_FRAME_BYTES);
  EXPECT_TRUE(!engine.pump(state, 20));

  EXPECT_TRUE(!engine.has_in_flight());
  EXPECT_EQ(speaker.stop_calls(), 1U);
  EXPECT_TRUE(!speaker.running());
  EXPECT_EQ(engine.played_frames(), 0U);
  // Only the 400 bytes accepted before the clear ever reached the speaker.
  EXPECT_EQ(speaker.received().size(), 400U);

  // Playback resumes cleanly for audio accepted after the clear.
  const AudioFrame third = filled_frame(0x33);
  engine.submit(third.bytes.data(), third.bytes.size(), state, token);
  EXPECT_TRUE(engine.pump(state, 20));
  EXPECT_EQ(engine.played_frames(), 1U);
  EXPECT_EQ(speaker.start_calls(), 2U);
}

void test_generation_change_invalidates_queued_and_in_flight_audio() {
  ProductionSessionState state{};
  FakeSpeaker speaker{};
  Engine engine{};
  engine.attach_speaker(&speaker);
  const SessionToken token = bring_ready(state);

  speaker.set_accept_limit(240);
  const AudioFrame first = filled_frame(0x41);
  engine.submit(first.bytes.data(), first.bytes.size(), state, token);
  engine.pump(state, 20);
  EXPECT_TRUE(engine.has_in_flight());

  const AudioFrame second = filled_frame(0x42);
  engine.submit(second.bytes.data(), second.bytes.size(), state, token);

  // A new session generation supersedes everything captured under the old one.
  state.stop();
  const SessionToken next_token = bring_ready(state);
  EXPECT_TRUE(next_token.generation != token.generation);

  speaker.set_accept_limit(PCM_FRAME_BYTES);
  engine.pump(state, 20);  // Drops the in-flight suffix and stops the speaker.
  engine.pump(state, 20);  // Drops the superseded queued frame.

  EXPECT_TRUE(!engine.has_in_flight());
  EXPECT_EQ(engine.played_frames(), 0U);
  EXPECT_EQ(engine.queued_frames(), 0U);
  EXPECT_EQ(speaker.stop_calls(), 1U);
  EXPECT_EQ(speaker.received().size(), 240U);
  EXPECT_EQ(engine.discarded_frames(), 2U);
}

void test_partial_writes_retain_only_the_unwritten_suffix() {
  ProductionSessionState state{};
  FakeSpeaker speaker{};
  Engine engine{};
  engine.attach_speaker(&speaker);
  const SessionToken token = bring_ready(state);

  speaker.set_accept_limit(400);
  const AudioFrame frame = filled_frame(0x55);
  engine.submit(frame.bytes.data(), frame.bytes.size(), state, token);

  EXPECT_TRUE(engine.pump(state, 20));
  EXPECT_EQ(engine.in_flight_offset(), 400U);
  EXPECT_TRUE(engine.pump(state, 20));
  EXPECT_EQ(engine.in_flight_offset(), 800U);
  EXPECT_TRUE(engine.pump(state, 20));

  EXPECT_TRUE(!engine.has_in_flight());
  EXPECT_EQ(engine.played_frames(), 1U);
  EXPECT_EQ(engine.partial_writes(), 2U);
  EXPECT_EQ(speaker.play_calls(), 3U);
  // Exactly one frame of audio reached the speaker: no prefix was replayed.
  EXPECT_EQ(speaker.received().size(), PCM_FRAME_BYTES);
  EXPECT_TRUE(std::equal(speaker.received().begin(), speaker.received().end(), frame.bytes.begin()));
}

void test_repeated_speaker_stalls_raise_a_latched_fault() {
  ProductionSessionState state{};
  FakeSpeaker speaker{};
  Engine engine{};
  engine.attach_speaker(&speaker);
  const SessionToken token = bring_ready(state);

  speaker.set_accept_limit(0);
  const AudioFrame frame = filled_frame(0x66);
  engine.submit(frame.bytes.data(), frame.bytes.size(), state, token);

  for (uint32_t i = 0; i < Engine::MAX_CONSECUTIVE_STALLS - 1; i++) {
    engine.pump(state, 20);
    EXPECT_TRUE(!engine.fault_pending());
  }
  engine.pump(state, 20);

  EXPECT_TRUE(engine.fault_pending());
  EXPECT_TRUE(engine.take_fault());
  EXPECT_TRUE(!engine.take_fault());  // The fault is consumed once.
  EXPECT_EQ(engine.played_frames(), 0U);
  EXPECT_EQ(speaker.received().size(), 0U);
}

void test_sustained_overflow_raises_a_latched_fault() {
  ProductionSessionState state{};
  FakeSpeaker speaker{};
  Engine engine{};
  engine.attach_speaker(&speaker);
  const SessionToken token = bring_ready(state);

  const AudioFrame frame = filled_frame(0x77);
  for (size_t i = 0; i < 3; i++)
    engine.submit(frame.bytes.data(), frame.bytes.size(), state, token);
  EXPECT_TRUE(!engine.fault_pending());

  for (uint32_t i = 0; i < Engine::MAX_CONSECUTIVE_OVERFLOWS; i++)
    engine.submit(frame.bytes.data(), frame.bytes.size(), state, token);

  EXPECT_TRUE(engine.take_fault());
  EXPECT_EQ(engine.overflow_frames(), Engine::MAX_CONSECUTIVE_OVERFLOWS);
  EXPECT_EQ(engine.queued_frames(), 3U);
}

void test_missing_speaker_never_dereferences_null() {
  ProductionSessionState state{};
  Engine engine{};
  const SessionToken token = bring_ready(state);

  const AudioFrame frame = filled_frame(0x88);
  EXPECT_TRUE(engine.submit(frame.bytes.data(), frame.bytes.size(), state, token) == Engine::Submit::ACCEPTED);
  EXPECT_TRUE(engine.pump(state, 20));
  EXPECT_EQ(engine.played_frames(), 0U);
  EXPECT_TRUE(!engine.has_in_flight());
}

}  // namespace

int main() {
  test_frames_are_rejected_before_ready_and_when_malformed();
  test_submit_never_touches_the_speaker();
  test_frames_play_in_order_one_per_pump();
  test_queue_is_bounded_and_drops_stale_first();
  test_clear_discards_queued_and_in_flight_audio_then_stops();
  test_generation_change_invalidates_queued_and_in_flight_audio();
  test_partial_writes_retain_only_the_unwritten_suffix();
  test_repeated_speaker_stalls_raise_a_latched_fault();
  test_sustained_overflow_raises_a_latched_fault();
  test_missing_speaker_never_dereferences_null();
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All respeaker_realtime playback tests passed\n";
  return EXIT_SUCCESS;
}

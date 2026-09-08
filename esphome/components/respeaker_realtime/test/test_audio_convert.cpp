#include "../audio_convert.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using esphome::respeaker_realtime::AudioFrame;
using esphome::respeaker_realtime::FragmentedTextAssembler;
using esphome::respeaker_realtime::AudioStreamConverter;
using esphome::respeaker_realtime::PcmFrameAssembler;
using esphome::respeaker_realtime::ProductionSessionState;
using esphome::respeaker_realtime::RawCadenceConverter;
using esphome::respeaker_realtime::RawFrameClock;
using esphome::respeaker_realtime::StaticStaleQueue;
using esphome::respeaker_realtime::convert_48k_stereo_s32_to_24k_mono_s16;
using esphome::respeaker_realtime::convert_48k_stereo_s32le_to_24k_mono_s16le;
using esphome::respeaker_realtime::credential_log_value;

namespace {

int failures = 0;

#define EXPECT_TRUE(value)                                                                                              \
  do {                                                                                                                  \
    if (!(value)) {                                                                                                     \
      std::cerr << __FILE__ << ':' << __LINE__ << ": expected true: " #value << '\n';                               \
      failures++;                                                                                                       \
    }                                                                                                                   \
  } while (0)

#define EXPECT_EQ(actual, expected)                                                                                     \
  do {                                                                                                                  \
    const auto actual_value = (actual);                                                                                 \
    const auto expected_value = (expected);                                                                             \
    if (!(actual_value == expected_value)) {                                                                            \
      std::cerr << __FILE__ << ':' << __LINE__ << ": expected " #actual " == " #expected << '\n';                  \
      failures++;                                                                                                       \
    }                                                                                                                   \
  } while (0)

void append_s32le(std::vector<uint8_t> &bytes, int32_t sample) {
  const uint32_t value = static_cast<uint32_t>(sample);
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
  bytes.push_back(static_cast<uint8_t>(value >> 16U));
  bytes.push_back(static_cast<uint8_t>(value >> 24U));
}

void append_stereo(std::vector<uint8_t> &bytes, int32_t channel_0, int32_t channel_1) {
  append_s32le(bytes, channel_0);
  append_s32le(bytes, channel_1);
}

void test_conversion_vectors() {
  std::vector<uint8_t> silence(960U * 2U * sizeof(int32_t), 0);
  std::array<int16_t, 480> output{};
  EXPECT_EQ(convert_48k_stereo_s32le_to_24k_mono_s16le(silence.data(), silence.size(), output.data(), output.size()),
            480U);
  for (const auto sample : output)
    EXPECT_EQ(sample, 0);

  std::vector<uint8_t> clipping;
  append_stereo(clipping, INT32_MAX, 0);
  append_stereo(clipping, 0, 0);
  append_stereo(clipping, INT32_MIN, 0);
  std::array<int16_t, 2> clipped{};
  EXPECT_EQ(convert_48k_stereo_s32_to_24k_mono_s16(clipping.data(), clipping.size(), clipped.data(), clipped.size()),
            2U);
  EXPECT_EQ(clipped[0], INT16_MAX);
  EXPECT_EQ(clipped[1], INT16_MIN);

  std::vector<uint8_t> channels;
  append_stereo(channels, 0x12340000, 0x43210000);
  append_stereo(channels, 0x11110000, 0x22220000);
  std::array<int16_t, 1> selected{};
  EXPECT_EQ(convert_48k_stereo_s32_to_24k_mono_s16(channels.data(), channels.size(), selected.data(), 1, 0), 1U);
  EXPECT_EQ(selected[0], 0x1234);
  EXPECT_EQ(convert_48k_stereo_s32_to_24k_mono_s16(channels.data(), channels.size(), selected.data(), 1, 1), 1U);
  EXPECT_EQ(selected[0], 0x4321);

  std::vector<uint8_t> ramp;
  for (int32_t frame = 0; frame < 8; frame++)
    append_stereo(ramp, frame << 16, (100 + frame) << 16);
  std::array<int16_t, 4> decimated{};
  EXPECT_EQ(convert_48k_stereo_s32_to_24k_mono_s16(ramp.data(), ramp.size(), decimated.data(), decimated.size()),
            4U);
  EXPECT_EQ(decimated[0], 0);
  EXPECT_EQ(decimated[1], 2);
  EXPECT_EQ(decimated[2], 4);
  EXPECT_EQ(decimated[3], 6);
}

void test_phase_is_carried_across_callbacks() {
  AudioStreamConverter converter(0);
  std::vector<uint8_t> first;
  std::vector<uint8_t> second;
  for (int32_t frame = 0; frame < 3; frame++)
    append_stereo(first, frame << 16, 0);
  for (int32_t frame = 3; frame < 6; frame++)
    append_stereo(second, frame << 16, 0);

  std::array<int16_t, 4> output{};
  EXPECT_EQ(converter.convert(first.data(), first.size(), output.data(), output.size()), 2U);
  EXPECT_EQ(converter.convert(second.data(), second.size(), output.data() + 2, output.size() - 2), 1U);
  EXPECT_EQ(output[0], 0);
  EXPECT_EQ(output[1], 2);
  EXPECT_EQ(output[2], 4);
}

void test_long_run_raw_cadence_and_callback_boundaries() {
  RawCadenceConverter converter(0);
  std::array<uint8_t, 85 * 8> full{};
  std::array<uint8_t, 43 * 8> half{};
  std::array<int16_t, 128> output{};
  size_t total = 0;
  for (size_t callback = 0; callback < 187; callback++)
    total += converter.convert(full.data(), full.size(), 256, output.data(), output.size());
  total += converter.convert(half.data(), half.size(), 128, output.data(), output.size());
  EXPECT_EQ(total, 24000U);
  EXPECT_EQ(converter.raw_phase(), 0U);

  RawFrameClock clock;
  size_t timed_total = 0;
  for (size_t callback = 0; callback <= 187; callback++)
    timed_total += clock.advance(callback * 1000000 / 187);
  EXPECT_EQ(timed_total, 48000U);

  converter.reset();
  total = converter.convert(half.data(), half.size(), 127, output.data(), output.size());
  total += converter.convert(half.data(), half.size(), 129, output.data(), output.size());
  EXPECT_EQ(total, 128U);
  EXPECT_EQ(converter.raw_phase(), 0U);
}

void test_raw_cadence_channel_and_sign() {
  RawCadenceConverter converter(1);
  std::vector<uint8_t> selected;
  append_stereo(selected, 0, 0x12340000);
  append_stereo(selected, 0, INT32_MIN);
  std::array<int16_t, 4> output{};
  EXPECT_EQ(converter.convert(selected.data(), selected.size(), 6, output.data(), output.size()), 3U);
  EXPECT_EQ(output[0], 0x1234);
  EXPECT_EQ(output[1], 0x1234);
  EXPECT_EQ(output[2], INT16_MIN);
}

void test_production_session_state_interleavings() {
  ProductionSessionState state;
  const auto first = state.start();
  EXPECT_TRUE(state.needs_reopen(0));
  EXPECT_TRUE(state.on_open(first));
  EXPECT_TRUE(!state.capture_send_token().valid);
  EXPECT_TRUE(state.on_hello(first));
  EXPECT_TRUE(state.on_ready(first));
  const auto send = state.capture_send_token();
  EXPECT_TRUE(send.valid);
  bool sent = false;
  EXPECT_TRUE(state.run_if_current(send, [&]() { sent = true; }));
  EXPECT_TRUE(sent);

  state.stop();
  EXPECT_TRUE(!state.may_send(send));
  sent = false;
  EXPECT_TRUE(!state.run_if_current(send, [&]() { sent = true; }));
  EXPECT_TRUE(!sent);
  const auto second = state.start();
  EXPECT_TRUE(second.generation != first.generation);
  EXPECT_TRUE(second.transport_epoch != first.transport_epoch);
  EXPECT_TRUE(state.needs_reopen(first.transport_epoch));
  EXPECT_TRUE(!state.on_ready(first));
  EXPECT_TRUE(state.on_open(second));
  EXPECT_TRUE(state.on_hello(second));
  EXPECT_TRUE(state.on_ready(second));

  const auto callback = state.capture_audio_token();
  state.advance_audio_epoch();
  EXPECT_TRUE(!state.may_emit(callback));
}

void test_fragmented_controls() {
  FragmentedTextAssembler assembler;
  EXPECT_EQ(assembler.append(0x01, false, 0, "{\"ty", 4), FragmentedTextAssembler::Result::INCOMPLETE);
  EXPECT_EQ(assembler.append(0x00, true, 0, "pe\":\"ready\"}", 12), FragmentedTextAssembler::Result::COMPLETE);
  EXPECT_EQ(std::string(assembler.data(), assembler.size()), std::string("{\"type\":\"ready\"}"));
  assembler.reset();
  EXPECT_EQ(assembler.append(0x00, true, 0, "bad", 3), FragmentedTextAssembler::Result::REJECTED);
}

void test_frame_size_and_cadence() {
  PcmFrameAssembler assembler;
  std::array<int16_t, 960> samples{};
  for (size_t i = 0; i < samples.size(); i++)
    samples[i] = static_cast<int16_t>(i);

  std::vector<AudioFrame> frames;
  assembler.append(samples.data(), 479, [&](const AudioFrame &frame) { frames.push_back(frame); });
  EXPECT_EQ(frames.size(), 0U);
  assembler.append(samples.data() + 479, 1, [&](const AudioFrame &frame) { frames.push_back(frame); });
  EXPECT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0].bytes.size(), 960U);
  assembler.append(samples.data() + 480, 480, [&](const AudioFrame &frame) { frames.push_back(frame); });
  EXPECT_EQ(frames.size(), 2U);
  EXPECT_EQ(frames[1].bytes[0], static_cast<uint8_t>(480));
  EXPECT_EQ(frames[1].bytes[1], static_cast<uint8_t>(480 >> 8U));
}

void test_bounded_queue_drops_stale_first() {
  StaticStaleQueue<int, 3> queue;
  EXPECT_TRUE(!queue.push(1));
  EXPECT_TRUE(!queue.push(2));
  EXPECT_TRUE(!queue.push(3));
  EXPECT_TRUE(queue.push(4));
  EXPECT_EQ(queue.dropped(), 1U);

  int value = 0;
  EXPECT_TRUE(queue.pop(value));
  EXPECT_EQ(value, 2);
  EXPECT_TRUE(queue.pop(value));
  EXPECT_EQ(value, 3);
  EXPECT_TRUE(queue.pop(value));
  EXPECT_EQ(value, 4);
  EXPECT_TRUE(!queue.pop(value));
  queue.push(5);
  queue.clear();
  EXPECT_EQ(queue.size(), 0U);
  EXPECT_EQ(queue.stale_cleared(), 1U);
}

void test_credentials_are_redacted() {
  const std::string secret = "device-credential-never-log";
  const std::string safe_value = credential_log_value(secret);
  EXPECT_EQ(safe_value, std::string("<redacted>"));
  EXPECT_TRUE(safe_value.find(secret) == std::string::npos);
}

}  // namespace

int main() {
  test_conversion_vectors();
  test_phase_is_carried_across_callbacks();
  test_long_run_raw_cadence_and_callback_boundaries();
  test_raw_cadence_channel_and_sign();
  test_production_session_state_interleavings();
  test_fragmented_controls();
  test_frame_size_and_cadence();
  test_bounded_queue_drops_stale_first();
  test_credentials_are_redacted();
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All respeaker_realtime native tests passed\n";
  return EXIT_SUCCESS;
}

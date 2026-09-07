#include "../audio_convert.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using esphome::respeaker_realtime::AudioFrame;
using esphome::respeaker_realtime::AudioStreamConverter;
using esphome::respeaker_realtime::MicrophoneCallbackConverter;
using esphome::respeaker_realtime::PcmFrameAssembler;
using esphome::respeaker_realtime::SessionLifecycle;
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
      std::cerr << __FILE__ << ':' << __LINE__ << ": expected " #actual " == " #expected << " (" << actual_value \
                << " != " << expected_value << ")\n";                                                               \
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

void test_actual_microphone_callback_resampling() {
  MicrophoneCallbackConverter converter(0);
  std::vector<uint8_t> input;
  append_stereo(input, 0 << 16, 100 << 16);
  append_stereo(input, 3 << 16, 103 << 16);
  append_stereo(input, 6 << 16, 106 << 16);

  std::array<int16_t, 5> output{};
  size_t count = 0;
  for (size_t offset = 0; offset < input.size(); offset += 8)
    count += converter.convert_frame(input.data() + offset, output.data() + count);
  EXPECT_EQ(count, 4U);
  EXPECT_EQ(output[0], 0);
  EXPECT_EQ(output[1], 2);
  EXPECT_EQ(output[2], 4);
  EXPECT_EQ(output[3], 6);

  converter.reset();
  converter.set_input_channel(1);
  int16_t selected[2]{};
  EXPECT_EQ(converter.convert_frame(input.data(), selected), 1U);
  EXPECT_EQ(selected[0], 100);
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

void test_session_ordering_and_reconnect() {
  SessionLifecycle lifecycle;
  EXPECT_TRUE(lifecycle.start(7));
  EXPECT_TRUE(!lifecycle.start(8));
  EXPECT_TRUE(!lifecycle.can_send_audio());
  EXPECT_TRUE(!lifecycle.on_ready(7));
  EXPECT_TRUE(lifecycle.on_connected());
  EXPECT_TRUE(!lifecycle.can_send_audio());
  lifecycle.on_hello_sent();
  EXPECT_TRUE(!lifecycle.on_ready(8));
  EXPECT_TRUE(lifecycle.on_ready(7));
  EXPECT_TRUE(lifecycle.can_send_audio());

  lifecycle.on_disconnected();
  EXPECT_TRUE(!lifecycle.can_send_audio());
  EXPECT_EQ(lifecycle.next_backoff_ms(), 250U);
  EXPECT_EQ(lifecycle.next_backoff_ms(), 500U);
  EXPECT_EQ(lifecycle.next_backoff_ms(), 1000U);
  EXPECT_EQ(lifecycle.next_backoff_ms(), 2000U);
  EXPECT_EQ(lifecycle.next_backoff_ms(), 4000U);
  EXPECT_EQ(lifecycle.next_backoff_ms(), 4000U);
  lifecycle.stop();
  EXPECT_TRUE(!lifecycle.active());
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
  test_actual_microphone_callback_resampling();
  test_frame_size_and_cadence();
  test_bounded_queue_drops_stale_first();
  test_session_ordering_and_reconnect();
  test_credentials_are_redacted();
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All respeaker_realtime native tests passed\n";
  return EXIT_SUCCESS;
}

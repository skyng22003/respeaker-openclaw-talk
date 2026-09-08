#!/usr/bin/env python3
"""Dependency-free host checks when the C++/ESPHome toolchains are unavailable.

The authoritative behavior tests are test_audio_convert.cpp. These checks keep
its deterministic vectors independently executable and verify security/framing
invariants in the source; they are not a substitute for compiling that test.
"""

from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "audio_convert.h").read_text()
PLAYBACK = (ROOT / "playback.h").read_text()
SOURCE = (ROOT / "respeaker_realtime.cpp").read_text()
SCHEMA = (ROOT / "__init__.py").read_text()
PACKAGE = (ROOT.parents[2] / "packages" / "realtime-talk.yaml").read_text()


def reference_convert(raw: bytes, channel: int = 0, phase: int = 0):
    output = []
    frames = len(raw) // 8
    for frame in range(frames):
        if phase == 0:
            sample = struct.unpack_from("<i", raw, frame * 8 + channel * 4)[0]
            shifted = sample // 65536
            output.append(max(-32768, min(32767, shifted)))
        phase ^= 1
    return output, phase


def stereo(*pairs):
    return b"".join(struct.pack("<ii", left, right) for left, right in pairs)


class AudioVectors(unittest.TestCase):
    def test_silence_cadence_and_frame_size(self):
        output, phase = reference_convert(bytes(960 * 8))
        self.assertEqual(len(output), 480)
        self.assertEqual(len(struct.pack("<480h", *output)), 960)
        self.assertEqual(set(output), {0})
        self.assertEqual(phase, 0)

    def test_channel_decimation_and_saturation(self):
        raw = stereo(
            (2**31 - 1, 0x12340000),
            (0x11110000, 0x22220000),
            (-2**31, 0x43210000),
            (0x33330000, 0x44440000),
        )
        self.assertEqual(reference_convert(raw, 0)[0], [32767, -32768])
        self.assertEqual(reference_convert(raw, 1)[0], [0x1234, 0x4321])

    def test_phase_carries_across_callback_boundaries(self):
        first = stereo(*[((i << 16), 0) for i in range(3)])
        second = stereo(*[((i << 16), 0) for i in range(3, 6)])
        first_out, phase = reference_convert(first)
        second_out, phase = reference_convert(second, phase=phase)
        self.assertEqual(first_out + second_out, [0, 2, 4])
        self.assertEqual(phase, 0)

    def test_one_second_raw_cadence_is_exact_across_callbacks(self):
        spans = [256] * 187 + [128]
        phase = total = 0
        for span in spans:
            total += (phase + span) // 2
            phase = (phase + span) % 2
        self.assertEqual((sum(spans), total, phase), (48000, 24000, 0))


class QueueAndLifecycleVectors(unittest.TestCase):
    def test_stale_first_overflow_and_stop_clear(self):
        queue = []
        dropped = 0
        for value in (1, 2, 3, 4):
            if len(queue) == 3:
                queue.pop(0)
                dropped += 1
            queue.append(value)
        self.assertEqual(queue, [2, 3, 4])
        self.assertEqual(dropped, 1)
        stale = len(queue)
        queue.clear()
        self.assertEqual((queue, stale), ([], 3))

    def test_reconnect_backoff_vector_is_bounded(self):
        backoffs = [min(250 << min(attempt, 4), 4000) for attempt in range(6)]
        self.assertEqual(backoffs, [250, 500, 1000, 2000, 4000, 4000])


class SourceContracts(unittest.TestCase):
    def test_temporary_home_assistant_session_controls(self):
        self.assertIn('name: "Start realtime session"', PACKAGE)
        self.assertIn('id(realtime_client).start_session("manual");', PACKAGE)
        self.assertIn('name: "Stop realtime session"', PACKAGE)
        compact_package = "".join(PACKAGE.split())
        self.assertIn(
            "id(realtime_client).stop_session(esphome::respeaker_realtime::StopReason::LOCAL_STOP);",
            compact_package,
        )

    def test_fixed_audio_contract_and_stale_first_queue(self):
        self.assertIn("PCM_SAMPLES_PER_FRAME = 480", HEADER)
        self.assertIn("PCM_FRAME_BYTES = PCM_SAMPLES_PER_FRAME * sizeof(int16_t)", HEADER)
        self.assertIn("this->head_ = (this->head_ + 1) % Capacity", HEADER)
        self.assertIn("this->dropped_++", HEADER)
        self.assertIn("this->stale_cleared_ += this->size_", HEADER)
        self.assertIn("class RawCadenceConverter", HEADER)
        self.assertIn("class ProductionSessionState", HEADER)

    def test_hello_gates_audio_and_reconnect_is_bounded(self):
        connected_case = SOURCE[SOURCE.index("case WEBSOCKET_EVENT_CONNECTED:") :]
        self.assertIn("!this->send_hello_()", connected_case)
        self.assertIn("this->hello_sent_.load()", SOURCE)
        self.assertIn("if (!this->session_state_.ready())", SOURCE)
        self.assertIn("MAX_RECONNECT_ATTEMPTS", SOURCE)
        self.assertIn("std::min<uint32_t>(250U << shift, 4000U)", SOURCE)
        self.assertIn("pdMS_TO_TICKS(5000)", SOURCE)
        self.assertIn("this->clear_uplink_queue_();", SOURCE)

    def test_schema_bounds_and_dependencies(self):
        self.assertIn('DEPENDENCIES = ["esp32", "network", "microphone", "speaker"]', SCHEMA)
        self.assertIn('value.startswith(("ws://", "wss://"))', SCHEMA)
        self.assertIn("value.total_seconds < 5 or value.total_seconds > 300", SCHEMA)
        self.assertIn("cv.int_range(min=0, max=1)", SCHEMA)
        self.assertIn('add_idf_component(name="espressif/esp_websocket_client", ref="1.6.1")', SCHEMA)

    def test_playback_never_runs_on_the_transport_callback(self):
        callback = SOURCE[SOURCE.index("void RespeakerRealtime::handle_playback_frame_") :]
        callback = callback[: callback.index("\n}\n")]
        self.assertIn("this->playback_.submit(", callback)
        for forbidden in ("speaker_->play", "speaker_->start", "speaker_->stop", "while ("):
            self.assertNotIn(forbidden, callback)
        # Only the dedicated playback task drives the speaker.
        self.assertIn('xTaskCreateStatic(playback_task_entry_, "realtime_play"', SOURCE)
        self.assertIn("this->playback_.pump(this->session_state_", SOURCE)
        self.assertNotIn("this->speaker_->play(", SOURCE)

    def test_playback_invalidation_precedes_speaker_stop(self):
        self.assertIn("this->clear_epoch_.fetch_add(1);", PLAYBACK)
        clear = PLAYBACK[PLAYBACK.index("void request_clear()") :]
        clear = clear[: clear.index("\n  }\n")]
        # request_clear invalidates and defers the stop to the playback task.
        self.assertIn("this->stop_requested_.store(true);", clear)
        self.assertNotIn("speaker_", clear)
        self.assertIn("frame.clear_epoch == this->clear_epoch_.load() && state.may_emit(frame.token)", PLAYBACK)
        # Every terminal path invalidates instead of playing stale audio.
        for terminal in ("stop_session", "fail_session_"):
            body = SOURCE[SOURCE.index(f"void RespeakerRealtime::{terminal}") :]
            self.assertIn("this->playback_.request_clear();", body[: body.index("\n}\n")])

    def test_playback_retains_only_the_unwritten_suffix(self):
        self.assertIn("this->current_.audio.bytes.data() + this->offset_", PLAYBACK)
        self.assertIn("const size_t remaining = PCM_FRAME_BYTES - this->offset_;", PLAYBACK)
        self.assertIn("this->offset_ += written;", PLAYBACK)
        self.assertIn("MAX_CONSECUTIVE_STALLS = 50", PLAYBACK)
        self.assertIn("MAX_CONSECUTIVE_OVERFLOWS = 25", PLAYBACK)
        self.assertIn("audio::AudioStreamInfo(16, 1, 24000)", SOURCE)

    def test_logs_redact_credentials_and_never_log_pcm(self):
        log_lines = [line for line in SOURCE.splitlines() if "ESP_LOG" in line]
        joined = "\n".join(log_lines)
        self.assertIn("credential_log_value", joined)
        self.assertNotIn("credential_.c_str()", joined)
        self.assertNotIn("frame.bytes", joined)
        self.assertNotIn("data.data()", joined)
        self.assertIn('return "<redacted>"', HEADER)


if __name__ == "__main__":
    unittest.main(verbosity=2)

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
VOICE_ASSISTANT = (ROOT.parents[2] / "packages" / "voice-assistant.yaml").read_text()
REALTIME_CONFIG = (ROOT.parents[2] / "config" / "respeaker-xvf-realtime-example.yaml").read_text()


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


class WakeRoutingContract(unittest.TestCase):
    """Wake-word -> session routing must not double-start two pipelines.

    Package merging APPENDS ``on_wake_word_detected`` actions, so a second
    handler in the realtime package would start both the Home Assistant voice
    assistant and the realtime session. The shared package therefore keeps the
    only handler and selects its final ordinary-wake action through a
    substitution that the realtime config overrides.
    """

    def wake_handler(self) -> str:
        handler = VOICE_ASSISTANT[VOICE_ASSISTANT.index("on_wake_word_detected:") :]
        return handler[: handler.index("\nvoice_assistant:")]

    def test_single_wake_handler_routes_through_substitution_hook(self):
        self.assertEqual(VOICE_ASSISTANT.count("on_wake_word_detected:"), 1)
        self.assertNotIn("on_wake_word_detected:", PACKAGE)
        self.assertIn("wake_session_script_id:", VOICE_ASSISTANT)
        self.assertIn("id: ${wake_session_script_id}", VOICE_ASSISTANT)
        # The ordinary-wake branch must go through the hook, not a bare start.
        self.assertNotIn("voice_assistant.start", self.wake_handler())

    def test_default_hook_keeps_legacy_voice_assistant_start(self):
        self.assertIn("wake_session_script_id: start_voice_session", VOICE_ASSISTANT)
        self.assertIn("id: start_voice_session", VOICE_ASSISTANT)
        self.assertIn("voice_assistant.start:", VOICE_ASSISTANT)
        self.assertIn("wake_word: !lambda return wake_word;", VOICE_ASSISTANT)

    def test_realtime_package_defines_session_script(self):
        self.assertIn("id: start_realtime_session", PACKAGE)
        self.assertIn("id(realtime_client).start_session(wake_word);", PACKAGE)

    def test_realtime_config_overrides_the_hook(self):
        self.assertIn("wake_session_script_id: start_realtime_session", REALTIME_CONFIG)
        self.assertIn("realtime_ref: feat/realtime-wake-routing", REALTIME_CONFIG)

    def test_realtime_config_sources_shared_hook_and_component_from_same_ref(self):
        self.assertGreaterEqual(
            REALTIME_CONFIG.count("url: https://github.com/skyng22003/respeaker-openclaw-talk"),
            2,
        )
        self.assertNotIn(
            "url: https://github.com/formatBCE/Respeaker-XVF3800-ESPHome-integration",
            REALTIME_CONFIG,
        )
        self.assertGreaterEqual(REALTIME_CONFIG.count("ref: ${realtime_ref}"), 2)
        self.assertIn(
            "url: https://github.com/formatBCE/Respeaker-XVF3800-ESPHome-integration",
            (ROOT.parents[2] / "packages" / "base.yaml").read_text(),
        )
        self.assertIn("external_components:\n  - !remove", REALTIME_CONFIG)
        self.assertIn("- respeaker_xvf3800\n      - aic3104", REALTIME_CONFIG)

    def test_priority_branches_are_preserved(self):
        for marker in (
            "switch.is_off: mic_mute_switch",
            "switch.is_on: timer_ringing",
            "switch.turn_off: timer_ringing",
            "media_player.is_announcing:",
            "switch.is_on: wake_sound",
            "switch.is_on: beam_lock_enabled",
            "id(respeaker).lock_beam();",
        ):
            self.assertIn(marker, VOICE_ASSISTANT)
        # The session hook sits after the wake sound and before the beam lock.
        hook = VOICE_ASSISTANT.index("id: ${wake_session_script_id}")
        sound = VOICE_ASSISTANT.index("switch.is_on: wake_sound")
        beam = VOICE_ASSISTANT.index("id(respeaker).lock_beam();")
        self.assertLess(sound, hook)
        self.assertLess(hook, beam)

    def test_duplicate_wake_starts_are_suppressed(self):
        start = SOURCE[SOURCE.index("void RespeakerRealtime::start_session") :]
        start = start[: start.index("\n}\n")]
        self.assertIn("compare_exchange_strong", start)
        # Model the latch: a second wake before Stop must not start again.
        requested = False
        starts = 0
        for _ in range(2):
            expected = False
            if expected == requested:
                requested = True
                starts += 1
        self.assertEqual(starts, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)

# Task 3 report — ESP32/XVF3800 microphone capture and bridge uplink

Date: 2026-09-07

Branch: `feature/realtime-talk-bridge`

Planned commit: `feat: stream XVF3800 microphone audio to realtime bridge`

## Scope and safety

Implemented the Task 3 software path only. No hardware was flashed, no service was deployed, no credentials were read, and no production/OpenClaw state was changed. `packages/hardware.yaml`, all board-fixed pins, and the XVF3800 internal-host DSP firmware `1.0.7` remain unchanged.

The existing `packages/voice-assistant.yaml` path is unchanged. Realtime transport is opt-in through `packages/realtime-talk.yaml`; the prototype configuration deliberately keeps the existing voice-assistant package until later lifecycle tasks integrate wake/stop behavior.

## RED evidence

1. The dependency-free source-presence assertion initially failed because the Task 3 conversion/lifecycle implementation did not exist.
2. The authoritative C++17 test was authored, but the environment has no `c++`, `g++`, or `clang++`; its compile/run could not be executed. No compiler or package was installed.

## Implementation

### Actual audio contract and conversion

Repository/source inspection found an important convention that differs from the raw I2S configuration:

- `packages/hardware.yaml` configures the XVF3800 bus as 48 kHz, stereo, 32-bit slots.
- The pinned `formatBCE/esphome@respeaker_microphone` microphone implementation internally retains every third 8-byte stereo frame before invoking ESPHome microphone callbacks and reports callback metadata as 16 kHz stereo S32LE.

The runtime therefore consumes the **actual callback contract** (nominal 16 kHz stereo S32LE), selects channel 0 by default, converts Q31/full-slot samples to signed PCM16, and linearly resamples 16→24 kHz. Treating the callback bytes as raw 48 kHz and decimating again would have produced approximately 8 kHz audio mislabeled as 24 kHz; that defect was corrected during review.

For the Task 3 unit-contract requirement, `convert_48k_stereo_s32le_to_24k_mono_s16le(...)` is also provided as a pure, allocation-free helper for raw 48 kHz stereo S32LE vectors.

Output is assembled as exactly 480 mono PCM16LE samples / 960 bytes / 20 ms per binary WebSocket message. Conversion state and partial input/output framing persist across callbacks without component-owned hot-path heap allocation.

### Queue and latency policy

- Six-frame fixed-capacity queue (120 ms at the negotiated format).
- Full queue drops the oldest frame before accepting the newest and increments a drop counter.
- Stop, disconnect/reconnect, and fresh readiness clear stale queued audio.
- Session generation and audio-epoch checks prevent an in-flight old callback from enqueueing into a new session.

### Transport and lifecycle

- Component-owned FreeRTOS uplink task plus Espressif WebSocket client task.
- Correct ESP-IDF managed-component registration via ESPHome `add_idf_component`, pinned to `espressif/esp_websocket_client` `1.6.1`.
- `ws://` and CA-bundle-verified `wss://` configuration; certificate-bundle support is explicitly enabled.
- Version-1 `hello` carries the device-specific credential and negotiated 24 kHz mono S16LE format.
- Binary audio is gated until a valid integral, matching-generation `ready` arrives after `hello` on the active connection.
- Fragmented inbound text controls, `ping`/`pong`, local stop, bridge close/error, send failure, and reconnect are handled.
- Consecutive reconnect failures are capped at five with bounded 250/500/1000/2000/4000 ms backoff; a successful `ready` resets the attempt budget.
- Credentials are schema-marked sensitive and logs emit only `<redacted>`; raw PCM and the full bridge URL are not logged.
- Sent/drop/stale/pre-ready/reconnect counters are available; opt-in diagnostic entities expose non-sensitive transport counters.

## Files

Created:

- `esphome/components/respeaker_realtime/__init__.py`
- `esphome/components/respeaker_realtime/audio_convert.h`
- `esphome/components/respeaker_realtime/respeaker_realtime.h`
- `esphome/components/respeaker_realtime/respeaker_realtime.cpp`
- `esphome/components/respeaker_realtime/test/test_audio_convert.cpp`
- `esphome/components/respeaker_realtime/test/test_host_contract.py`
- `packages/realtime-talk.yaml`
- `config/respeaker-xvf-realtime-example.yaml`
- this report

Modified:

- `packages/base.yaml` — makes the component available without enabling it.
- `config/secrets.yaml.example` — placeholder-only device/bridge settings.
- `docs/realtime-talk-bridge.md` — software verification and explicit no-flash boundary.

## Verification

Passed:

- `python3 esphome/components/respeaker_realtime/test/test_host_contract.py`
  - 10/10 deterministic checks, including raw 48→24 vectors, actual nominal 16→24 callback resampling, channel selection, signed limits, phase continuity, 960-byte framing, stale-first queue behavior, bounded reconnect, hello/readiness gating, managed-component registration, schema bounds, and logging redaction.
- `python3 -m py_compile esphome/components/respeaker_realtime/__init__.py esphome/components/respeaker_realtime/test/test_host_contract.py`
- `git diff --check` and `git diff --cached --check`
- Task 2 regression: `cd bridge && npm test` — 3 files, 46/46 tests passed.
- Task 2 regression: `cd bridge && npm run build` — TypeScript build passed.
- Source/API review against ESPHome `2026.6.0`, `formatBCE/esphome@respeaker_microphone`, Espressif `esp_websocket_client` `1.6.1`, and the checked-in Task 2 bridge protocol/server.
- Invariant diff: no changes to `packages/hardware.yaml`, XVF3800 component sources, or the `1.0.7` DSP firmware binary.

Unavailable locally:

- Native C++17 compile/run: no `c++`, `g++`, or `clang++` is installed.
- ESPHome schema/code generation and full ESP-IDF compile: no `esphome`, PlatformIO, or ESP-IDF toolchain is installed.

No compiler/toolchain was installed and no unapproved container was created. The Python/source checks are useful evidence but do not replace the blocked native and firmware builds.

## Security and self-review

- No credential value, Gateway token, Wi-Fi secret, OTA secret, API key, or raw PCM was added or logged.
- The device credential is distinct from any OpenClaw Gateway credential and remains in ignored local secrets.
- Generated `bridge/node_modules/` and `bridge/dist/` are untracked and excluded from the commit.
- Callback-owned conversion state is no longer reset concurrently by transport/main-loop code.
- Matching generation, active connection, sent `hello`, requested session, and `ready` all gate audio.
- Queue capacity, send timeout, network timeout, retry count, and backoff are finite.

## Unvalidated hardware/compiler items

Hardware validation remains pending and unauthorized:

- compile against the exact ESPHome 2026.6.0 / ESP-IDF graph and confirm managed WebSocket, cJSON, and certificate-bundle linkage;
- verify on-device callback metadata, channel ordering, 32-bit slot alignment, and the pinned fork's callback cadence (its current per-read every-third-frame implementation may introduce a small rate discrepancy relative to nominal 16 kHz metadata);
- inspect captured PCM for level, polarity, clipping, interpolation quality, and exact long-run 20 ms framing cadence;
- validate LAN `ws://` and certificate-verified `wss://` handshakes;
- validate sustained queue pressure, reconnect, stale-drop behavior, memory/stack headroom, mute behavior, and watchdog stability;
- validate coexistence with existing ESPHome/Home Assistant voice-assistant behavior;
- perform end-to-end bridge authentication and audio inspection without logging raw PCM;
- flashing/upload and every physical-device test require separate explicit approval.

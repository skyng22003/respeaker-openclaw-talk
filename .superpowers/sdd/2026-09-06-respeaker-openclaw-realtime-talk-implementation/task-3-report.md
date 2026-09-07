# Task 3 report — ESP32/XVF3800 microphone capture and bridge uplink

Date: 2026-09-07

Branch: `feature/realtime-talk-bridge`

Planned commit: `feat: stream XVF3800 microphone audio to realtime bridge`

## Scope and safety

Implemented the Task 3 software path only. No hardware was flashed, no service was deployed, no credentials were read, and no production/OpenClaw state was changed. `packages/hardware.yaml`, all board-fixed pins, and the XVF3800 internal-host DSP firmware `1.0.7` remain unchanged.

The existing `packages/voice-assistant.yaml` path is unchanged. Realtime transport is opt-in through `packages/realtime-talk.yaml`; the prototype configuration deliberately keeps the existing voice-assistant package until later lifecycle tasks integrate wake/stop behavior.

## RED evidence

1. Initial Task 3 RED: the dependency-free source-presence assertion failed because the conversion/lifecycle implementation did not exist.
2. Fix Round 1 RED: new production-contract assertions failed because `RawCadenceConverter` and `ProductionSessionState` did not exist. Native tests were also changed first to reference those missing types, including immediate restart, stop-during-send, in-flight epoch advance, fragmented controls, and one-second cadence vectors.
3. Fix Round 1 GREEN: the dependency-free suite passes with production using those extracted types. The authoritative C++17 test remains uncompiled because the environment has no `c++`, `g++`, or `clang++`; no compiler or package was installed.

## Implementation

### Actual audio contract and conversion

Repository/source inspection found an important convention that differs from the raw I2S configuration:

- `packages/hardware.yaml` configures the XVF3800 bus as 48 kHz, stereo, 32-bit slots.
- The pinned `formatBCE/esphome@respeaker_microphone` microphone implementation internally retains every third 8-byte stereo frame before invoking ESPHome microphone callbacks and reports callback metadata as 16 kHz stereo S32LE.

The callback metadata says 16 kHz, but its bytes are not an exact continuous 16 kHz stream: each nominal 2,048-byte raw read restarts an every-third selector, produces 85 stereo frames, and discards the raw remainder. The runtime therefore does **not** apply a blind 3:2 ratio. It uses a monotonic microsecond clock with integer remainder to recover raw 48 kHz elapsed-frame cadence, then emits exactly one PCM16 sample per two elapsed raw frames. Missing selector samples are deterministically held from the most recent selected value. The rational phase and timestamp remainder persist across callback boundaries.

For the Task 3 raw-vector contract, `convert_48k_stereo_s32le_to_24k_mono_s16le(...)` remains a pure, allocation-free helper. A one-second long-run vector proves that 48,000 elapsed raw frames produce exactly 24,000 output samples across 187 full callback spans plus a boundary span.

Output is assembled as exactly 480 mono PCM16LE samples / 960 bytes / 20 ms per binary WebSocket message. Conversion, clock remainder, and framing state persist across callbacks without component-owned hot-path heap allocation.

### Queue and latency policy

- Six-frame fixed-capacity queue (120 ms at the negotiated format).
- Full queue drops the oldest frame before accepting the newest and increments a drop counter.
- Stop, disconnect/reconnect, and fresh readiness clear stale queued audio.
- Production `SessionToken` fences include session generation, transport epoch, and audio epoch; an in-flight callback can enqueue only while all three remain current.

### Transport and lifecycle

- Component-owned FreeRTOS uplink task plus Espressif WebSocket client task.
- Correct ESP-IDF managed-component registration via ESPHome `add_idf_component`, pinned to `espressif/esp_websocket_client` `1.6.1`.
- `ws://` and CA-bundle-verified `wss://` configuration; certificate-bundle support is explicitly enabled.
- Version-1 `hello` carries the device-specific credential and negotiated 24 kHz mono S16LE format.
- Every start advances generation/transport epoch; the transport-owning task must close any old socket and open/hello the new epoch before readiness.
- Binary audio is gated until a valid integral, matching-generation `ready` arrives after `hello` on the active connection. The production state fence is rechecked immediately around the bounded binary-send call, so stop invalidates an already-popped frame.
- Fragmented inbound text assembly is extracted and host-testable; `ping`/`pong`, local stop, bridge close/error, send failure, and reconnect are handled.
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
  - 10/10 deterministic checks, including exact one-second raw cadence, channel selection, signed limits, phase continuity, 960-byte framing, stale-first queue behavior, bounded reconnect, hello/readiness gating, managed-component registration, schema bounds, and logging redaction.
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
- The production `ProductionSessionState`, `RawCadenceConverter`, `RawFrameClock`, and `FragmentedTextAssembler` are the same host-testable state machinery used by `RespeakerRealtime`; the unused lifecycle surrogate was removed.
- Matching generation, transport epoch, audio epoch, active connection, sent `hello`, requested session, and `ready` all gate audio.
- Queue capacity, send timeout, network timeout, retry count, and backoff are finite.

## Unvalidated hardware/compiler items

Hardware validation remains pending and unauthorized:

- compile against the exact ESPHome 2026.6.0 / ESP-IDF graph and confirm managed WebSocket, cJSON, and certificate-bundle linkage;
- verify on-device callback metadata, channel ordering, 32-bit slot alignment, callback timestamps, and the pinned fork's per-read every-third behavior;
- inspect captured PCM for level, polarity, clipping, sample-hold quality, timestamp jitter response, and exact long-run 20 ms framing cadence;
- validate LAN `ws://` and certificate-verified `wss://` handshakes;
- validate sustained queue pressure, reconnect, stale-drop behavior, memory/stack headroom, mute behavior, and watchdog stability;
- validate coexistence with existing ESPHome/Home Assistant voice-assistant behavior;
- perform end-to-end bridge authentication and audio inspection without logging raw PCM;
- flashing/upload and every physical-device test require separate explicit approval.

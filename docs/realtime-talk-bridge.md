# Realtime Talk bridge

This project will add an ESP32-to-LAN bridge for OpenClaw Realtime Talk while
preserving the existing ESPHome/Home Assistant satellite as the rollback
target. It does not require or permit an OpenClaw update.

## Firmware safety gate

No prototype firmware may be uploaded until all of these conditions are true:

- the Sky-attested deployed top-level YAML and its matching secrets-free remote
  package bytes remain preserved in the durable private backup;
- the preserved factory image remains present in the durable private backup and
  passes its SHA-256 manifest check;
- the embedded ESPHome core version and exact package bytes/hashes are recorded;
- every exported file passes the external SHA-256 manifest check; and
- an exact restore command appropriate to the captured binary type and device
  transport is recorded and reviewed.

`secrets.yaml`, API keys, OTA passwords, Wi-Fi credentials, Gateway tokens, and
bridge credentials must never be copied into the repository or checksum
documentation. The ESP32 receives only its device-specific bridge credential;
the OpenClaw Gateway credential remains protected on the host-side bridge and
must never be placed on the ESP32.

The practical rollback baseline is complete and ready for re-review. See
[`artifacts/rollback/restore.md`](../artifacts/rollback/restore.md) for verified
repository evidence, exact restore methods, and the historical mutable-`main`
commit caveat. This documentation does not itself authorize firmware upload.

## Hardware invariants

The prototype must preserve the board-fixed I2C/I2S pins, XVF3800 microphone
input, AIC3104-backed speaker chain, mute behavior, wake/`Stop` models, and LED
control. The board-fixed pins and `packages/hardware.yaml` XVF3800 internal-host
DSP firmware `1.0.7` are immutable constraints for this project.

## Task 3 software-only verification

The pure raw-I2S conversion, actual pinned-fork callback resampling, framing,
stale-first queue, lifecycle ordering, reconnect, and credential-redaction checks are in
`esphome/components/respeaker_realtime/test/`. On a host with a C++17 compiler,
run the authoritative native test with:

```bash
mkdir -p /tmp/respeaker-task3-test
c++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  esphome/components/respeaker_realtime/test/test_audio_convert.cpp \
  -o /tmp/respeaker-task3-test/test_audio_convert
/tmp/respeaker-task3-test/test_audio_convert
```

The dependency-free fallback check can be run without ESPHome or a C++ compiler:

```bash
python3 esphome/components/respeaker_realtime/test/test_host_contract.py
```

For full validation, temporarily use the repository-local `esphome/components`
as the external-component source in a local, uncommitted copy of the example,
then run:

```bash
esphome config config/respeaker-xvf-realtime-example.yaml
esphome compile config/respeaker-xvf-realtime-example.yaml
```

Neither compile command uploads firmware. Firmware upload and hardware audio,
cadence, reconnect, and sustained-operation validation remain blocked pending
explicit flash approval.

## Recovery acceptance checks

A rollback is successful only when the restored device boots reliably,
connects to Home Assistant, reports XVF3800 firmware `1.0.7`, detects the local
wake and `Stop` words, honors microphone mute, plays audio through the existing
speaker path, restores normal LED state, and completes an ordinary Home
Assistant satellite request/response. Re-run the source manifest after the
restore; rollback artifacts must remain byte-for-byte unchanged.

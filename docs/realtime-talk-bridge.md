# Realtime Talk bridge

This project will add an ESP32-to-LAN bridge for OpenClaw Realtime Talk while
preserving the existing ESPHome/Home Assistant satellite as the rollback
target. It does not require or permit an OpenClaw update.

## Firmware safety gate

No prototype firmware may be uploaded until all of these conditions are true:

- the exact currently deployed top-level YAML and its complete secrets-free
  local package graph have been exported outside Git;
- the preserved factory image remains present in the durable private backup and
  passes its SHA-256 manifest check;
- the ESPHome version and source repository commit/ref used for the deployed
  build are recorded;
- every exported file passes the external SHA-256 manifest check; and
- an exact restore command appropriate to the captured binary type and device
  transport is recorded and reviewed.

`secrets.yaml`, API keys, OTA passwords, Wi-Fi credentials, Gateway tokens, and
bridge credentials must never be copied into the repository or checksum
documentation. The ESP32 receives only its device-specific bridge credential;
the OpenClaw Gateway credential remains protected on the host-side bridge and
must never be placed on the ESP32.

The initial read-only capture is incomplete. See
[`artifacts/rollback/restore.md`](../artifacts/rollback/restore.md) for verified
repository evidence and the precise blockers. This means the firmware gate is
currently **closed**.

## Hardware invariants

The prototype must preserve the board-fixed I2C/I2S pins, XVF3800 microphone
input, AIC3104-backed speaker chain, mute behavior, wake/`Stop` models, and LED
control. The board-fixed pins and `packages/hardware.yaml` XVF3800 internal-host
DSP firmware `1.0.7` are immutable constraints for this project.

## Recovery acceptance checks

A rollback is successful only when the restored device boots reliably,
connects to Home Assistant, reports XVF3800 firmware `1.0.7`, detects the local
wake and `Stop` words, honors microphone mute, plays audio through the existing
speaker path, restores normal LED state, and completes an ordinary Home
Assistant satellite request/response. Re-run the source manifest after the
restore; rollback artifacts must remain byte-for-byte unchanged.

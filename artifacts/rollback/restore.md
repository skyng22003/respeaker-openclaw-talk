# Rollback baseline

## Gate status

**Hardware upload is blocked.** A flashable ESP32 factory image is now preserved
and verified, but the exact deployed device YAML, its secrets-free local
include graph, the deployed ESPHome version, and the deployed source commit/ref
remain unavailable. The supplied source ZIP contains the upstream example, not
proof of the deployed configuration.

The factory image provides a viable binary recovery path. It does not make a
future prototype build reproducible or prove which configuration produced the
working deployment. No prototype compile or upload may be treated as
flash-ready until those missing inputs are exported without copying
`secrets.yaml`, added to the durable rollback set, and pass its manifest check.

## Verified repository baseline

- Repository commit: `b0e07d832c495a8fdd334f0cb3717296d78b1132`
- Repository branch at capture: `feature/realtime-talk-bridge`
- Repository manifest: `artifacts/rollback/manifest.sha256`
- Durable private backup:
  `/mnt/user/Array/Backup/AgentBackup/reSpeaker/OpenClaw-Talk/2026-09-06/`
- Committed verification manifest for that backup:
  `artifacts/rollback/durable-backup.sha256`
- ESPHome project/minimum version declared by `packages/base.yaml`: `2026.6.0`
- Actual ESPHome version used by the deployed build: **unknown**
- Actual repository commit/ref used by the deployed build: **unknown**

Verify the committed public baseline from the repository root:

```bash
sha256sum --check artifacts/rollback/manifest.sha256
```

Verify the durable private backup on Unraid through the approved management
route:

```bash
ssh -i /root/.openclaw/keys/eve-unraid-management root@192.168.3.100 \
  'cd /mnt/user/Array/Backup/AgentBackup/reSpeaker/OpenClaw-Talk/2026-09-06 && sha256sum --check backup.sha256'
```

The durable set contains the factory image, upstream source ZIP, supplied
23-file source manifest, provenance record, and `backup.sha256`. It contains no
Wi-Fi, Home Assistant API, OTA, bridge, or OpenClaw Gateway credentials.

## Verified ESP32 factory image

The private binary
`respeaker-xvf3800-assistant-firmware.factory.bin` is 3,192,496 bytes with
SHA-256:

```text
1dc03d21528e56bea183def108585e7654c32544f5b369036631e8a9dd7fcd30
```

Read-only parsing established that it is a merged ESP32 factory image:

- valid ESP image at offset `0x0`;
- partition-table magic at `0x8000` with five entries;
- first declared app partition at `0x10000`; and
- a valid six-segment ESP application image at `0x10000` ending within the
  supplied binary.

These observations establish the image type; they are not instructions to
guess or manually supply flash offsets. Use ESPHome Web's factory-image flow
below.

## Fixed DSP baseline

`packages/hardware.yaml` remains pinned to XVF3800 internal-host firmware
`1.0.7` with declared MD5 `043a848f544ff2c7265ac19685daf5de`.
The repository binary is:

```text
SHA-256 d1132b7e779818923072396315304a5dd2324aa56969b4111b30678f968a7c6a
MD5     043a848f544ff2c7265ac19685daf5de
Size    888832 bytes
```

This is the XMOS DSP image embedded by the ESPHome component. It is **not** a
flashable ESP32 rollback image.

## Required capture before any prototype upload

Use a read-only/non-secret export route to create a new timestamped directory
outside Git. Preserve:

- the deployed top-level YAML;
- every local secrets-free YAML/package it includes;
- the exact ESPHome version; and
- the exact repository commit/ref used by that build.

Do not copy `secrets.yaml`. Build the manifest without hashing the manifest
itself, then verify it:

```bash
find "$ROLLBACK_DIR" -type f ! -name manifest.sha256 -print0 \
  | sort -z | xargs -0 sha256sum > "$ROLLBACK_DIR/manifest.sha256"
(cd "$ROLLBACK_DIR" && sha256sum --check manifest.sha256)
```

The brief's unfiltered `find "$ROLLBACK_DIR" -type f ...` form includes the
redirect-created manifest in its own input and therefore cannot produce a
self-verifying checksum. The exclusion above is required.

## Restore procedure

1. Stop. Verify the durable rollback set with `sha256sum --check backup.sha256`.
   A missing file or non-zero result forbids the upload.
2. Copy only the verified factory binary from the private backup to the local
   computer performing the restore. Keep it outside Git.
3. Connect the XIAO ESP32-S3 directly over USB to a Chromium browser that
   supports Web Serial and open <https://web.esphome.io/>.
4. Select **Connect**, choose the board's serial port, select **Install**, then
   choose the verified
   `respeaker-xvf3800-assistant-firmware.factory.bin` file and confirm the
   installation. This ESPHome Web factory-image flow handles the merged image;
   do not enter a manual offset or use the XMOS DSP binary.
5. Wait for ESPHome Web to report completion before disconnecting or resetting
   the board. If ESPHome Web cannot connect or rejects the image, stop; do not
   substitute an unreviewed command or offset.
6. After restore, validate all of the following before declaring recovery:
   device boot without a crash loop; Home Assistant API connection; local wake
   word and `Stop`; microphone mute; XVF3800 version `1.0.7`; speaker output;
   LED states; ordinary Home Assistant voice request/response; and a second
   manifest check proving the source artifacts were unchanged.

The binary restore method is established, but until the exact deployed YAML and
build provenance are captured, the prototype flash gate remains closed.

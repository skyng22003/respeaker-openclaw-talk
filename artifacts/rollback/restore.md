# Rollback baseline

## Gate status

**Rollback baseline ready for re-review.** A flashable ESP32 factory image and
Sky-attested top-level deployed YAML are preserved and verified. Its five remote package
files byte-match the supplied source ZIP and this checkout at
`b0e07d832c495a8fdd334f0cb3717296d78b1132`. The YAML selected mutable ref
`main`, however, so this byte match does not prove which historical commit
Device Builder fetched. Embedded factory-image metadata establishes that the
deployed image uses ESPHome core `2026.8.2`.

The merged factory image provides byte-for-byte recovery through the documented
CLI or GUI procedure. The attested YAML and exact package bytes provide an
immutable configuration baseline even though historical remote commit identity
behind mutable `main` is unprovable. This satisfies the practical Task 1
rollback contract and is ready for re-review; it does not authorize a prototype
flash, which still requires explicit review/approval. No secret-bearing
configuration is part of this baseline.

## Verified repository baseline

- Repository commit: `b0e07d832c495a8fdd334f0cb3717296d78b1132`
- Repository branch at capture: `feature/realtime-talk-bridge`
- Repository manifest: `artifacts/rollback/manifest.sha256`
- Durable private backup:
  `/mnt/user/Array/Backup/AgentBackup/reSpeaker/OpenClaw-Talk/2026-09-06/`
- Committed verification manifest for that backup:
  `artifacts/rollback/durable-backup.sha256`
- Sky-attested YAML supplement manifest:
  `artifacts/rollback/durable-backup-fix-round-2.sha256`
- Factory metadata evidence manifest:
  `artifacts/rollback/durable-backup-fix-round-3.sha256`
- ESPHome project/minimum version declared by `packages/base.yaml`: `2026.6.0`
- Home Assistant Device Builder add-on version: `1.13.1` (this is **not** proof
  of the underlying ESPHome core version)
- ESPHome core version embedded in the deployed factory image: `2026.8.2`
- Project/component version embedded in the image: `2026.6.0`
- Configured repository ref: `main` (mutable; exact historical commit unknown)

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

The non-destructive Fix Round 2 supplement adds:

- `deployed-respeaker-xvf-satellite-example.yaml` — 1,494 bytes, 47 lines,
  SHA-256
  `0f4c0d4403ea367628e4568b539eda0deb80070cf0120e7d0c583e2b2535d990`;
- `provenance-fix-round-2.txt`; and
- `backup-fix-round-2.sha256`.

Sky attests that this is the top-level YAML used for the deployed device. It
exactly matches the YAML in the supplied ZIP and checkout. Its remote package
graph selects the `formatBCE/Respeaker-XVF3800-ESPHome-integration` repository,
ref `main`, refresh `1d`, and `base`, `hardware`, `voice-assistant`, `leds`, and
`timers-alarm` packages. All five supplied package files exactly match this
checkout's baseline bytes and hashes. This establishes a preserved,
attested configuration byte set; it does not convert mutable `main` into proof
of the commit historically fetched by Device Builder.

The non-destructive Fix Round 3 supplement adds
`factory-metadata-evidence.txt` and `backup-fix-round-3.sha256`. The evidence is
bound to the factory image's SHA-256 and contains only allowlisted printable
version/device identity strings—no raw binary contents, credentials, network
values, or unrelated strings. It records:

```text
ESPHome version 2026.8.2 compiled on %s
Project formatbce.Respeaker XVF3800 Satellite version 2026.6.0
respeaker-xvf3800-assistant
reSpeaker XVF3800 Assistant
```

Therefore `2026.8.2` is the deployed ESPHome core version, `2026.6.0` is the
project/component version, and `1.13.1` is only the Home Assistant Device
Builder add-on version.

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

## Historical provenance caveat

If a read-only/non-secret source becomes available, preserve the exact
historical repository commit resolved from mutable ref `main`. Do not copy
`secrets.yaml`. The missing remote commit identity does not prevent rollback:
the factory image, embedded core version, attested top-level YAML, and exact
package bytes/hashes are independently preserved and immutable.

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

### Exact esptool alternative

Espressif's official ESP32-S3 esptool documentation defines `write-flash` as
offset/file pairs and says `--chip` is optional because the chip is detected.
For this structurally verified merged factory image, the complete image starts
at offset `0x0`. After verifying the private backup and replacing only the
serial-port placeholder, the exact command is:

```bash
esptool --chip esp32s3 --port <SERIAL_PORT> write-flash 0x0 \
  respeaker-xvf3800-assistant-firmware.factory.bin
```

Run it only from a directory containing the verified private factory binary.
Do not add offsets, split the image, use the XMOS DSP binary, or substitute an
OTA image. ESPHome Web remains the safer GUI alternative.

Byte-for-byte firmware rollback is available. Task 1 is ready for re-review;
prototype flashing remains prohibited until the next task's explicit safety
review and approval.

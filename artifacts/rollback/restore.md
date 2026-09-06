# Rollback baseline

## Gate status

**Hardware upload is blocked.** The read-only inspection on 2026-09-06 did not
find either of the two artifacts required to restore the currently deployed
ESP32 image:

1. the exact deployed device YAML, including its secrets-free local include
   graph; and
2. a flashable export of the currently deployed ESP32 firmware.

The repository snapshot below is verified evidence, but it is not evidence of
what is currently deployed. No prototype compile or upload may be treated as
flash-ready until both missing artifacts are exported without copying
`secrets.yaml`, added to an external rollback set, and pass that set's manifest
check.

## Verified repository baseline

- Repository commit: `b0e07d832c495a8fdd334f0cb3717296d78b1132`
- Repository branch at capture: `feature/realtime-talk-bridge`
- Repository manifest: `artifacts/rollback/manifest.sha256`
- External repository-only snapshot:
  `/root/.openclaw/workspace/tmp/respeaker-openclaw-talk-rollback/20260906T225648Z`
- ESPHome project/minimum version declared by `packages/base.yaml`: `2026.6.0`
- Actual ESPHome version used by the deployed build: **unknown**
- Actual repository commit/ref used by the deployed build: **unknown**

Verify the committed public baseline from the repository root:

```bash
sha256sum --check artifacts/rollback/manifest.sha256
```

Verify the external repository-only snapshot:

```bash
cd /root/.openclaw/workspace/tmp/respeaker-openclaw-talk-rollback/20260906T225648Z
sha256sum --check manifest.sha256
```

The external set contains only public repository inputs and provenance. It does
not contain Wi-Fi, API, OTA, or bridge credentials.

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
- the flashable ESP32 binary (and its type/offset or the complete ESPHome build
  directory needed to upload it);
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

1. Stop. Verify the complete deployed rollback set with `sha256sum --check`.
   A missing file or non-zero result forbids the upload.
2. Confirm the preserved binary type and upload method recorded at capture.
   Do not infer a flash offset from the filename.
3. Connect by the same transport recorded at capture (USB serial is preferred
   for recovery). Do not use an OTA command unless the preserved artifact was
   explicitly captured as an OTA image.
4. Run the exact upload command recorded with the exported build. **No exact
   safe command can be supplied yet**, because the deployed YAML, ESPHome build
   metadata, binary type, device address/serial path, and flash layout were not
   available through a read-only route.
5. After restore, validate all of the following before declaring recovery:
   device boot without a crash loop; Home Assistant API connection; local wake
   word and `Stop`; microphone mute; XVF3800 version `1.0.7`; speaker output;
   LED states; ordinary Home Assistant voice request/response; and a second
   manifest check proving the source artifacts were unchanged.

Until the missing exports and exact upload command are recorded, the only safe
rollback action is **do not flash the prototype**.

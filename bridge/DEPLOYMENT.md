# Local deployment review checklist

This document prepares a reviewable deployment only. It does **not** authorize
deployment, exposure, firmware upload, or device testing.

## Host-side secret boundary

Create `/etc/respeaker-talk-bridge/env` with mode `0600`, owned by the service
account. Copy the variable names from `.env.example` but use host-managed secret
entry for `GATEWAY_TOKEN` and `DEVICE_CREDENTIAL`. Do not put either in source,
logs, systemd unit files, shell history, or chat.

`GATEWAY_URL` must point to the private Gateway listener. Initially bind the
bridge to a private LAN address only after firewall review; `127.0.0.1` is the
safe default for host-only tests. The ESP32 needs the resulting private bridge
address/port and `DEVICE_CREDENTIAL`; it never receives `GATEWAY_TOKEN`.

## Pre-start checks

1. Run `npm ci`, `npm test`, and `npm run build` from `bridge/`.
2. Copy only `dist/`, `package.json`, `package-lock.json`, and production node
   dependencies into a versioned deployment directory.
3. Prefer the separate `compose.yaml` deployment. It builds an isolated image;
   the host-private environment file is mounted at runtime and never baked in.
   The included systemd unit remains a fallback only.
4. Restrict the listening firewall rule to the ESP32's fixed DHCP reservation.
   Do not expose the port to WAN or broader VLANs.
5. Start only after an explicit deployment approval, then verify `GET /healthz`
   locally before allowing the device to connect.

## First device test

The firmware test requires a separate explicit flash approval. Begin with a
short local-network session, inspect redacted service logs for authentication
and Talk session lifecycle only, and confirm playback clear/barge-in. Never log
raw audio or credentials.

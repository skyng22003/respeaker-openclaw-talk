# Task 2 Report — Authenticated LAN Bridge Core

**Date:** 2026-09-07
**Branch:** `feature/realtime-talk-bridge`
**Commit:** `feat: add authenticated realtime satellite bridge` (this task commit; SHA is reported after creation)

## Scope completed

Implemented only the Task 2 bridge core:

- strict Node 22 ESM TypeScript package with exact pinned dependencies and npm lockfile
- strict Zod device-control parsing for `hello`, `barge_in`, `stop`, and `pong`
- strict bridge-control schemas for `ready`, `activity`, `clear`, `error`, `close`, and `ping`
- protocol version/generation fencing and fixed 24 kHz mono PCM16 hello negotiation
- PCM binary validation (non-empty, even-byte aligned, maximum 3840 bytes)
- stale-first fixed-capacity FIFO queue
- authenticated WebSocket server using equal-length SHA-256 digests and `timingSafeEqual`
- `maxPayload: 4096`, `/healthz` count-only response, and no credential/audio logging
- hello-before-audio enforcement, policy close `1008` for bad credentials, and the injected `openTalk(DeviceHello)` seam
- one active generation per device; newer generations close the prior session before Talk is opened
- stale-generation control ignoring, generation-matched audio/control forwarding, and owner-checked map cleanup
- idempotent session closure and graceful closure of both authenticated and unauthenticated sockets
- process signal handling in the entry point

No Gateway adapter, ESP32 code, deployment, firmware operation, dependency outside `bridge/`, or OpenClaw change was made.

## Pre-edit verification

Command:

```text
git branch --show-current
git status --short
```

Output:

```text
feature/realtime-talk-bridge
```

`git status --short` produced no output, confirming a clean starting tree.

## TDD evidence

### Protocol — RED

Command:

```text
cd bridge && npm test -- protocol.test.ts
```

Result: exit 1.

```text
FAIL  test/protocol.test.ts
Error: Cannot find module '../src/protocol.js'
Test Files  1 failed (1)
```

### Protocol — GREEN

Command:

```text
npm test -- protocol.test.ts
```

Result: exit 0.

```text
✓ test/protocol.test.ts (8 tests)
Test Files  1 passed (1)
Tests       8 passed (8)
```

### Bounded queue — RED

Command:

```text
npm test -- bounded-queue.test.ts
```

Result: exit 1.

```text
FAIL  test/bounded-queue.test.ts
Error: Cannot find module '../src/bounded-queue.js'
Test Files  1 failed (1)
```

### Protocol + bounded queue — GREEN

Command:

```text
npm test -- protocol.test.ts bounded-queue.test.ts
```

Result: exit 0.

```text
✓ test/bounded-queue.test.ts (4 tests)
✓ test/protocol.test.ts (8 tests)
Test Files  2 passed (2)
Tests       12 passed (12)
```

### Authenticated server — RED

Command:

```text
npm test -- server-auth.test.ts
```

Result: exit 1.

```text
FAIL  test/server-auth.test.ts
Error: Cannot find module '../src/server.js'
Test Files  1 failed (1)
```

### Authenticated server — GREEN

Command:

```text
npm test -- server-auth.test.ts
```

Result: exit 0.

```text
✓ test/server-auth.test.ts (7 tests)
Test Files  1 passed (1)
Tests       7 passed (7)
```

### Graceful shutdown edge case — RED

A follow-up review identified that an unauthenticated WebSocket could hold shutdown open. A regression test was added before the fix.

Command:

```text
npm test -- server-auth.test.ts
```

Result: exit 1.

```text
× closes unauthenticated sockets during graceful server shutdown
AssertionError: expected 'timeout' to be 1001
Test Files  1 failed (1)
Tests       1 failed | 7 passed (8)
```

### Graceful shutdown edge case — GREEN

Command:

```text
npm test -- server-auth.test.ts
```

Result: exit 0.

```text
✓ test/server-auth.test.ts (8 tests)
Test Files  1 passed (1)
Tests       8 passed (8)
```

## Final verification

### Full test suite

Command:

```text
npm test
```

Result: exit 0.

```text
✓ test/bounded-queue.test.ts (4 tests)
✓ test/protocol.test.ts (8 tests)
✓ test/server-auth.test.ts (8 tests)
Test Files  3 passed (3)
Tests       20 passed (20)
```

### Build / strict typecheck

Command:

```text
npm run build
```

Result: exit 0.

```text
> tsc -p tsconfig.json
```

### Dependency install/audit

`npm install --include=dev` generated `package-lock.json`; npm reported 48 audited packages and 0 vulnerabilities. All requested direct dependencies and dev dependencies are exact versions rather than ranges.

## Files

Created:

- `bridge/package.json`
- `bridge/package-lock.json`
- `bridge/tsconfig.json`
- `bridge/src/config.ts`
- `bridge/src/protocol.ts`
- `bridge/src/bounded-queue.ts`
- `bridge/src/server.ts`
- `bridge/src/index.ts`
- `bridge/test/protocol.test.ts`
- `bridge/test/bounded-queue.test.ts`
- `bridge/test/server-auth.test.ts`
- `.superpowers/sdd/2026-09-06-respeaker-openclaw-realtime-talk-implementation/task-2-report.md`

Generated `bridge/dist/` and `bridge/node_modules/` are not part of the commit.

## Self-review

- Authentication hashes both candidate strings before constant-time comparison, avoiding length-dependent comparison behavior.
- Credentials are used only for validation and dependency injection; neither credentials nor raw PCM are logged.
- Authentication completes before `openTalk` or PCM acceptance.
- Session activation is serialized per device to prevent concurrent hellos from bypassing generation ownership.
- A newer generation awaits closure of the current Talk port before becoming the map owner and receiving `ready`.
- Closing callbacks delete a device only when the closing object remains the map owner.
- Message handling is serialized per socket, preserving PCM/control ordering and preventing overlapping lifecycle transitions.
- Session closure and Talk closure are idempotent.
- Graceful shutdown closes authenticated sessions plus pre-authentication sockets before closing the WebSocket and HTTP servers.
- Health output contains only `{ activeSessions }`; no device IDs or secret-bearing state are exposed.
- No real Gateway behavior is present: the entry-point seam intentionally returns a safe `talk_unavailable` path until Task 5 supplies the adapter.

## Concerns / follow-up boundaries

1. The available verification runtime was Node `v24.19.0`, not Node 22. The package engine is deliberately constrained to `>=22 <23`, TypeScript targets ES2023/Node ESM, and no Node 24-only API is used, but an actual Node 22 execution should be included in deployment/CI validation.
2. The Task 2 entry point cannot open a real Talk session by design; Task 5 must inject the Gateway-backed `openTalk` implementation.
3. LAN interface restriction is configuration-driven (`BRIDGE_HOST`, default loopback). Task 7 deployment must bind the intended private/LAN interface and avoid public ingress.
4. `ws://` transport is acceptable only on the trusted LAN per the approved prototype design; TLS/mTLS remains a later hardening option.

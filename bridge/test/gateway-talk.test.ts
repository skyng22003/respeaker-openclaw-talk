import { once } from "node:events";
import { generateKeyPairSync } from "node:crypto";
import { createServer } from "node:http";
import type { AddressInfo } from "node:net";

import { afterEach, describe, expect, it, vi } from "vitest";
import { WebSocketServer } from "ws";

import { openGatewayTalk } from "../src/gateway-talk.js";
import type { DeviceHello } from "../src/protocol.js";

const servers: Array<{ server: ReturnType<typeof createServer>; wss: WebSocketServer }> = [];

const device: DeviceHello = {
  type: "hello", version: 1, generation: 3, deviceId: "kitchen", credential: "test",
  sampleRate: 24_000, channels: 1, sampleFormat: "s16le", frameMs: 20,
  idleTimeoutSeconds: 30, firmware: "test",
};

async function startGateway(
  onRequest: (socket: import("ws").WebSocket, request: Record<string, unknown>) => void,
  challenge: { nonce: string; ts: number } = { nonce: "nonce", ts: Date.now() },
) {
  const server = createServer();
  const wss = new WebSocketServer({ server });
  wss.on("connection", (socket) => {
    socket.on("message", (raw) => onRequest(socket, JSON.parse(raw.toString()) as Record<string, unknown>));
    setImmediate(() => socket.send(JSON.stringify({
      type: "event", event: "connect.challenge", payload: challenge,
    })));
  });
  servers.push({ server, wss });
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const { port } = server.address() as AddressInfo;
  return `ws://127.0.0.1:${port}`;
}

function response(socket: import("ws").WebSocket, request: Record<string, unknown>, payload: unknown = { ok: true }) {
  socket.send(JSON.stringify({ type: "res", id: request.id, ok: true, payload }));
}

afterEach(async () => {
  await Promise.all(servers.splice(0).map(({ server, wss }) => new Promise<void>((resolve) => {
    for (const socket of wss.clients) socket.terminate();
    server.close(() => resolve());
  })));
});

describe("Gateway Talk adapter", () => {
  it("signs the Gateway challenge as the paired backend device", async () => {
    const { publicKey, privateKey } = generateKeyPairSync("ed25519");
    const publicKeyPem = publicKey.export({ type: "spki", format: "pem" });
    const privateKeyPem = privateKey.export({ type: "pkcs8", format: "pem" });
    const requests: Record<string, unknown>[] = [];
    const challengeTs = 1_789_000_000_000;
    const url = await startGateway((socket, request) => {
      requests.push(request);
      if (request.method === "connect") return response(socket, request);
      if (request.method === "talk.session.create") return response(socket, request, { sessionId: "relay-1" });
      if (request.method === "talk.session.close") return response(socket, request);
    }, { nonce: "challenge-nonce", ts: challengeTs });
    const callbacks = { audio: vi.fn(), clear: vi.fn(), activity: vi.fn(), failure: vi.fn() };
    const talk = await openGatewayTalk({
      url,
      token: "device-token",
      bootstrapToken: "bootstrap-token",
      sessionKey: "main",
      deviceIdentity: { deviceId: "device-id", publicKeyPem, privateKeyPem },
    }, device, callbacks);

    const connect = requests[0]?.params as Record<string, unknown>;
    expect(connect.auth).toEqual({ token: "bootstrap-token", deviceToken: "device-token" });
    expect(connect.device).toMatchObject({ id: "device-id", signedAt: challengeTs, nonce: "challenge-nonce" });
    expect((connect.device as Record<string, unknown>).publicKey).toBe(
      publicKey.export({ type: "spki", format: "der" }).subarray(-32).toString("base64url"),
    );
    expect(typeof (connect.device as Record<string, unknown>).signature).toBe("string");
    await talk.close();
  });

  it("authenticates, creates Talk, relays audio, cancels, and closes", async () => {
    const requests: Record<string, unknown>[] = [];
    const url = await startGateway((socket, request) => {
      requests.push(request);
      if (request.method === "connect") return response(socket, request);
      if (request.method === "talk.session.create") return response(socket, request, { sessionId: "relay-1" });
      if (request.method === "talk.session.appendAudio") return response(socket, request);
      if (request.method === "talk.session.cancelOutput") return response(socket, request, { status: "applied" });
      if (request.method === "talk.session.close") return response(socket, request);
    });
    const callbacks = { audio: vi.fn(), clear: vi.fn(), activity: vi.fn(), failure: vi.fn() };
    const talk = await openGatewayTalk({ url, token: "gateway-token", sessionKey: "main" }, device, callbacks);
    await talk.appendAudio(Buffer.alloc(960, 1));
    await talk.cancelOutput("barge-in");
    await talk.close();

    expect(requests.map((request) => request.method)).toEqual([
      "connect", "talk.session.create", "talk.session.appendAudio", "talk.session.cancelOutput", "talk.session.close",
    ]);
    expect(requests[0]?.params).toMatchObject({
      auth: { token: "gateway-token" },
      minProtocol: 4,
      maxProtocol: 4,
      client: { id: "gateway-client", mode: "backend" },
    });
    expect(requests[0]?.params).not.toHaveProperty("nonce");
    expect(requests[1]?.params).toMatchObject({ mode: "realtime", transport: "gateway-relay", brain: "agent-consult", sessionKey: "main" });
    expect(requests[2]?.params).toMatchObject({ sessionId: "relay-1", audioBase64: Buffer.alloc(960, 1).toString("base64") });
    expect(callbacks.clear).toHaveBeenCalledOnce();
  });

  it("forwards only matching relay audio and clear events", async () => {
    const url = await startGateway((socket, request) => {
      if (request.method === "connect") return response(socket, request);
      if (request.method === "talk.session.create") {
        response(socket, request, { sessionId: "relay-1" });
        setTimeout(() => {
          socket.send(JSON.stringify({ type: "event", event: "talk.event", payload: { relaySessionId: "other", type: "audio", audioBase64: Buffer.alloc(960, 2).toString("base64") } }));
          socket.send(JSON.stringify({ type: "event", event: "talk.event", payload: { relaySessionId: "relay-1", type: "audio", audioBase64: Buffer.alloc(960, 3).toString("base64") } }));
          socket.send(JSON.stringify({ type: "event", event: "talk.event", payload: { relaySessionId: "relay-1", type: "clear" } }));
        }, 20);
      }
      if (request.method === "talk.session.close") return response(socket, request);
    });
    const callbacks = { audio: vi.fn(), clear: vi.fn(), activity: vi.fn(), failure: vi.fn() };
    const talk = await openGatewayTalk({ url, token: "gateway-token", sessionKey: "main" }, device, callbacks);
    await vi.waitFor(() => expect(callbacks.audio).toHaveBeenCalledOnce());
    expect(callbacks.audio).toHaveBeenCalledWith(Buffer.alloc(960, 3));
    expect(callbacks.clear).toHaveBeenCalledOnce();
    await talk.close();
  });
});

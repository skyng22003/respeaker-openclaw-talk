import WebSocket, { type RawData } from "ws";

import type { DeviceHello } from "./protocol.js";
import type { TalkCallbacks, TalkPort } from "./server.js";

interface GatewayResponse {
  type: "res";
  id: string;
  ok: boolean;
  payload?: unknown;
  error?: { message?: string };
}

interface GatewayEvent {
  type: "event";
  event: string;
  payload?: unknown;
}

export interface GatewayTalkConfig {
  url: string;
  token: string;
  sessionKey: string;
  requestTimeoutMs?: number;
}

type Pending = { resolve(value: unknown): void; reject(reason: Error): void; timer: NodeJS.Timeout };

function parseFrame(data: RawData): unknown {
  const text = Buffer.isBuffer(data) ? data.toString("utf8") : data.toString();
  return JSON.parse(text) as unknown;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isResponse(value: unknown): value is GatewayResponse {
  return isRecord(value) && value.type === "res" && typeof value.id === "string" && typeof value.ok === "boolean";
}

function isEvent(value: unknown): value is GatewayEvent {
  return isRecord(value) && value.type === "event" && typeof value.event === "string";
}

function decodePcm(value: unknown): Buffer | undefined {
  if (typeof value !== "string" || value.length === 0 || value.length % 4 !== 0) return undefined;
  const pcm = Buffer.from(value, "base64");
  return pcm.length > 0 && pcm.length % 2 === 0 ? pcm : undefined;
}

export async function openGatewayTalk(
  config: GatewayTalkConfig,
  device: DeviceHello,
  callbacks: TalkCallbacks,
): Promise<TalkPort> {
  const socket = new WebSocket(config.url);
  const pending = new Map<string, Pending>();
  const timeoutMs = config.requestTimeoutMs ?? 10_000;
  let requestNumber = 0;
  let sessionId: string | undefined;
  let activeTurnId: string | undefined;
  let connected = false;
  let closed = false;
  let resolveConnected: (() => void) | undefined;
  let rejectConnected: ((reason: Error) => void) | undefined;

  const rejectPending = (message: string): void => {
    for (const entry of pending.values()) {
      clearTimeout(entry.timer);
      entry.reject(new Error(message));
    }
    pending.clear();
  };

  const request = async (method: string, params: Record<string, unknown>): Promise<unknown> => {
    if (!connected || socket.readyState !== WebSocket.OPEN) throw new Error("Gateway is disconnected");
    const id = `respeaker-${++requestNumber}`;
    return await new Promise<unknown>((resolve, reject) => {
      const timer = setTimeout(() => {
        pending.delete(id);
        reject(new Error(`Gateway request timed out: ${method}`));
      }, timeoutMs);
      pending.set(id, { resolve, reject, timer });
      socket.send(JSON.stringify({ type: "req", id, method, params }));
    });
  };

  const handleEvent = (event: GatewayEvent): void => {
    if (event.event === "connect.challenge") {
      const payload = isRecord(event.payload) ? event.payload : {};
      const id = `respeaker-${++requestNumber}`;
      socket.send(JSON.stringify({ type: "req", id, method: "connect", params: {
        minProtocol: 4,
        maxProtocol: 4,
        client: { id: "respeaker-talk-bridge", version: "0.1.0", platform: process.platform, mode: "backend" },
        caps: [],
        auth: { token: config.token },
        role: "operator",
        scopes: ["operator.talk"],
        nonce: payload.nonce,
      } }));
      const timer = setTimeout(() => {
        pending.delete(id);
        rejectConnected?.(new Error("Gateway authentication timed out"));
      }, timeoutMs);
      pending.set(id, {
        resolve: () => { connected = true; resolveConnected?.(); },
        reject: (error) => { callbacks.failure("gateway_auth_failed"); rejectConnected?.(error); socket.close(); },
        timer,
      });
      return;
    }
    if (event.event !== "talk.event" || !isRecord(event.payload)) return;
    const payload = event.payload;
    if (payload.relaySessionId !== sessionId) return;
    if (payload.type === "audio") {
      const pcm = decodePcm(payload.audioBase64);
      if (pcm !== undefined) callbacks.audio(pcm);
      return;
    }
    if (payload.type === "clear") {
      callbacks.clear();
      return;
    }
    const talkEvent = isRecord(payload.talkEvent) ? payload.talkEvent : undefined;
    if (talkEvent?.type === "output.audio.delta" && typeof talkEvent.turnId === "string") {
      activeTurnId = talkEvent.turnId;
      callbacks.activity("speaking");
    } else if (talkEvent?.type === "input.audio.delta") {
      callbacks.activity("listening");
    } else if (talkEvent?.type === "turn.end") {
      activeTurnId = undefined;
      callbacks.activity("listening");
    }
  };

  const handleMessage = (data: RawData): void => {
    let frame: unknown;
    try { frame = parseFrame(data); } catch { callbacks.failure("gateway_protocol_error"); return; }
    if (isResponse(frame)) {
      const entry = pending.get(frame.id);
      if (entry === undefined) return;
      pending.delete(frame.id);
      clearTimeout(entry.timer);
      if (frame.ok) entry.resolve(frame.payload);
      else entry.reject(new Error(frame.error?.message ?? "Gateway request rejected"));
      return;
    }
    if (isEvent(frame)) handleEvent(frame);
  };
  socket.on("message", handleMessage);
  socket.on("error", () => callbacks.failure("gateway_transport_error"));
  socket.on("close", () => {
    connected = false;
    rejectPending("Gateway connection closed");
    rejectConnected?.(new Error("Gateway connection closed"));
    if (!closed) callbacks.failure("gateway_disconnected");
  });

  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("Gateway connect challenge timed out")), timeoutMs);
    resolveConnected = () => { clearTimeout(timer); resolve(); };
    rejectConnected = (error) => { clearTimeout(timer); reject(error); };
    socket.once("error", (error) => rejectConnected?.(error));
  });
  const created = await request("talk.session.create", {
    mode: "realtime", transport: "gateway-relay", brain: "agent-consult",
    sessionKey: config.sessionKey, ttlMs: device.idleTimeoutSeconds * 1_000,
  });
  if (!isRecord(created) || typeof created.sessionId !== "string") {
    socket.close();
    throw new Error("Gateway returned no Talk session id");
  }
  sessionId = created.sessionId;
  callbacks.activity("listening");

  return {
    appendAudio: async (pcm) => {
      await request("talk.session.appendAudio", { sessionId, audioBase64: pcm.toString("base64") });
    },
    cancelOutput: async () => {
      const result = await request("talk.session.cancelOutput", { sessionId, turnId: activeTurnId, reason: "barge-in" });
      callbacks.clear();
      if (isRecord(result) && (result.status === "applied" || result.status === "stale" || result.status === "idle")) return result.status;
      return "idle";
    },
    close: async () => {
      if (closed) return;
      closed = true;
      try { if (sessionId !== undefined && connected) await request("talk.session.close", { sessionId }); }
      finally { socket.close(); }
    },
  };
}

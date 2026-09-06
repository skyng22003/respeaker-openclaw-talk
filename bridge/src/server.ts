import { createHash, timingSafeEqual } from "node:crypto";
import http, { type Server } from "node:http";

import WebSocket, { WebSocketServer, type RawData } from "ws";

import {
  parseControl,
  validatePcmFrame,
  type BridgeControl,
  type DeviceControl,
  type DeviceHello,
} from "./protocol.js";

export interface TalkPort {
  appendAudio(pcm: Buffer): Promise<void>;
  cancelOutput(reason: "barge-in"): Promise<"applied" | "stale" | "idle">;
  close(): Promise<void>;
}

export interface BridgeDependencies {
  credential: string;
  openTalk(device: DeviceHello): Promise<TalkPort>;
}

export interface DeviceSessionPort {
  readonly deviceId: string;
  readonly generation: number;
  close(code?: number, reason?: string, controlReason?: string): Promise<void>;
}

function credentialsMatch(supplied: string, configured: string): boolean {
  const suppliedDigest = createHash("sha256").update(supplied).digest();
  const configuredDigest = createHash("sha256").update(configured).digest();
  return timingSafeEqual(suppliedDigest, configuredDigest);
}

function asBuffer(data: RawData): Buffer {
  if (Buffer.isBuffer(data)) return data;
  if (data instanceof ArrayBuffer) return Buffer.from(data);
  if (Array.isArray(data)) return Buffer.concat(data);
  throw new TypeError("Unsupported WebSocket frame representation");
}

function sendControl(socket: WebSocket, control: BridgeControl): void {
  if (socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify(control));
  }
}

class DeviceSession implements DeviceSessionPort {
  readonly deviceId: string;
  readonly generation: number;
  readonly #socket: WebSocket;
  readonly #talk: TalkPort;
  readonly #onClosed: (session: DeviceSession) => void;
  #closing: Promise<void> | undefined;

  constructor(
    hello: DeviceHello,
    socket: WebSocket,
    talk: TalkPort,
    onClosed: (session: DeviceSession) => void,
  ) {
    this.deviceId = hello.deviceId;
    this.generation = hello.generation;
    this.#socket = socket;
    this.#talk = talk;
    this.#onClosed = onClosed;
  }

  appendAudio(pcm: Buffer): Promise<void> {
    return this.#talk.appendAudio(pcm);
  }

  cancelOutput(): Promise<"applied" | "stale" | "idle"> {
    return this.#talk.cancelOutput("barge-in");
  }

  close(code = 1000, reason = "session closed", controlReason?: string): Promise<void> {
    if (this.#closing !== undefined) return this.#closing;

    this.#closing = (async () => {
      if (controlReason !== undefined) {
        sendControl(this.#socket, {
          type: "close",
          version: 1,
          generation: this.generation,
          reason: controlReason,
        });
      }
      if (
        this.#socket.readyState === WebSocket.OPEN ||
        this.#socket.readyState === WebSocket.CONNECTING
      ) {
        this.#socket.close(code, reason);
      }
      try {
        await this.#talk.close();
      } finally {
        this.#onClosed(this);
      }
    })();

    return this.#closing;
  }
}

export function createBridgeServer(deps: BridgeDependencies): Server {
  const sessions = new Map<string, DeviceSession>();
  const activationTails = new Map<string, Promise<void>>();

  const server = http.createServer((request, response) => {
    if (request.method === "GET" && request.url === "/healthz") {
      response.writeHead(200, { "content-type": "application/json" });
      response.end(JSON.stringify({ activeSessions: sessions.size }));
      return;
    }
    response.writeHead(404).end();
  });

  const webSockets = new WebSocketServer({ server, maxPayload: 4_096 });

  async function serializeActivation(deviceId: string, operation: () => Promise<void>): Promise<void> {
    const predecessor = activationTails.get(deviceId) ?? Promise.resolve();
    let release = (): void => undefined;
    const gate = new Promise<void>((resolve) => {
      release = resolve;
    });
    const tail = predecessor.then(() => gate);
    activationTails.set(deviceId, tail);

    await predecessor;
    try {
      await operation();
    } finally {
      release();
      if (activationTails.get(deviceId) === tail) {
        activationTails.delete(deviceId);
      }
    }
  }

  webSockets.on("connection", (socket) => {
    let activeSession: DeviceSession | undefined;
    let messageChain = Promise.resolve();

    const closePolicy = (reason: string): void => {
      if (socket.readyState === WebSocket.OPEN) socket.close(1008, reason);
    };

    const activate = async (hello: DeviceHello): Promise<void> => {
      if (!credentialsMatch(hello.credential, deps.credential)) {
        closePolicy("authentication failed");
        return;
      }

      await serializeActivation(hello.deviceId, async () => {
        const current = sessions.get(hello.deviceId);
        if (current !== undefined) {
          if (hello.generation <= current.generation) {
            closePolicy("stale generation");
            return;
          }
          await current.close(1000, "superseded", "superseded");
        }

        if (socket.readyState !== WebSocket.OPEN) return;

        let talk: TalkPort;
        try {
          talk = await deps.openTalk(hello);
        } catch {
          sendControl(socket, {
            type: "error",
            version: 1,
            generation: hello.generation,
            code: "talk_unavailable",
          });
          socket.close(1011, "Talk unavailable");
          return;
        }

        if (socket.readyState !== WebSocket.OPEN) {
          await talk.close();
          return;
        }

        const session = new DeviceSession(hello, socket, talk, (closed) => {
          if (sessions.get(closed.deviceId) === closed) {
            sessions.delete(closed.deviceId);
          }
        });
        activeSession = session;
        sessions.set(hello.deviceId, session);
        sendControl(socket, {
          type: "ready",
          version: 1,
          generation: hello.generation,
        });
      });
    };

    const handleControl = async (control: DeviceControl): Promise<void> => {
      if (activeSession === undefined) {
        if (control.type !== "hello") {
          closePolicy("hello required");
          return;
        }
        await activate(control);
        return;
      }

      if (control.type === "hello") {
        closePolicy("hello already received");
        return;
      }
      if (control.generation !== activeSession.generation) return;

      if (control.type === "barge_in") {
        await activeSession.cancelOutput();
      } else if (control.type === "stop") {
        await activeSession.close(1000, "device stop", "device_stop");
      }
    };

    const handleMessage = async (data: RawData, isBinary: boolean): Promise<void> => {
      if (isBinary) {
        if (activeSession === undefined) {
          closePolicy("hello required");
          return;
        }
        await activeSession.appendAudio(validatePcmFrame(asBuffer(data)));
        return;
      }

      const control = parseControl(asBuffer(data).toString("utf8"));
      await handleControl(control);
    };

    socket.on("message", (data, isBinary) => {
      messageChain = messageChain
        .then(() => handleMessage(data, isBinary))
        .catch(() => {
          if (activeSession !== undefined) {
            void activeSession.close(1011, "session error", "session_error");
          } else {
            closePolicy("invalid message");
          }
        });
    });

    socket.on("close", () => {
      if (activeSession !== undefined) void activeSession.close();
    });

    socket.on("error", () => {
      if (activeSession !== undefined) void activeSession.close(1011, "transport error");
    });
  });

  const nativeClose = server.close.bind(server);
  let shutdown: Promise<void> | undefined;
  server.close = ((callback?: (error?: Error) => void): Server => {
    shutdown ??= (async () => {
      await Promise.all(
        [...sessions.values()].map((session) =>
          session.close(1001, "server shutdown", "server_shutdown"),
        ),
      );
      for (const socket of webSockets.clients) {
        if (
          socket.readyState === WebSocket.OPEN ||
          socket.readyState === WebSocket.CONNECTING
        ) {
          socket.close(1001, "server shutdown");
        }
      }
      await new Promise<void>((resolve) => webSockets.close(() => resolve()));
    })();

    void shutdown.then(
      () => {
        if (server.listening) nativeClose(callback);
        else callback?.();
      },
      (error: unknown) => callback?.(error instanceof Error ? error : new Error("shutdown failed")),
    );
    return server;
  }) as Server["close"];

  return server;
}

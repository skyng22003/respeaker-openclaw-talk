import { once } from "node:events";
import type { AddressInfo } from "node:net";
import { connect as connectTcp } from "node:net";

import WebSocket from "ws";
import { afterEach, describe, expect, it, vi } from "vitest";

import {
  createBridgeServer,
  type BridgeDependencies,
  type TalkPort,
} from "../src/server.js";

const configuredCredential = "correct-device-credential";
const servers: ReturnType<typeof createBridgeServer>[] = [];
const clients: WebSocket[] = [];

function fakeTalk(): TalkPort {
  return {
    appendAudio: vi.fn(async () => undefined),
    cancelOutput: vi.fn(async () => "idle" as const),
    close: vi.fn(async () => undefined),
  };
}

async function startServer(deps: Partial<BridgeDependencies> = {}) {
  const server = createBridgeServer({
    credential: configuredCredential,
    openTalk: vi.fn(async () => fakeTalk()),
    ...deps,
  });
  servers.push(server);
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const { port } = server.address() as AddressInfo;
  return { server, port };
}

async function connect(port: number): Promise<WebSocket> {
  const socket = new WebSocket(`ws://127.0.0.1:${port}`);
  clients.push(socket);
  await once(socket, "open");
  return socket;
}

function hello(generation: number, credential = configuredCredential) {
  return {
    type: "hello",
    version: 1,
    generation,
    deviceId: "kitchen-xvf3800",
    credential,
    sampleRate: 24_000,
    channels: 1,
    sampleFormat: "s16le",
    frameMs: 20,
    idleTimeoutSeconds: 30,
    firmware: "test",
  };
}

async function nextJson(socket: WebSocket): Promise<Record<string, unknown>> {
  const [data] = (await once(socket, "message")) as [WebSocket.RawData, boolean];
  return JSON.parse(data.toString()) as Record<string, unknown>;
}

async function nextClose(socket: WebSocket): Promise<number> {
  const [code] = (await once(socket, "close")) as [number, Buffer];
  return code;
}

function deferred(): { promise: Promise<void>; resolve: () => void } {
  let resolve = (): void => undefined;
  const promise = new Promise<void>((done) => {
    resolve = done;
  });
  return { promise, resolve };
}

afterEach(async () => {
  for (const client of clients.splice(0)) {
    if (client.readyState === WebSocket.OPEN) client.close();
  }
  await Promise.all(
    servers.splice(0).map(
      (server) =>
        new Promise<void>((resolve) => {
          if (!server.listening) return resolve();
          server.close(() => resolve());
        }),
    ),
  );
});

describe("authenticated bridge server", () => {
  it("rejects binary audio before hello without opening Talk", async () => {
    const openTalk = vi.fn(async () => fakeTalk());
    const { port } = await startServer({ openTalk });
    const socket = await connect(port);

    const closed = nextClose(socket);
    socket.send(Buffer.alloc(960));

    await expect(closed).resolves.toBe(1008);
    expect(openTalk).not.toHaveBeenCalled();
  });

  it("closes bad credentials with policy code 1008", async () => {
    const openTalk = vi.fn(async () => fakeTalk());
    const { port } = await startServer({ openTalk });
    const socket = await connect(port);

    const closed = nextClose(socket);
    socket.send(JSON.stringify(hello(1, "wrong-credential")));

    await expect(closed).resolves.toBe(1008);
    expect(openTalk).not.toHaveBeenCalled();
  });

  it("opens Talk once and sends ready for a valid hello", async () => {
    const talk = fakeTalk();
    const openTalk = vi.fn(async () => talk);
    const { port } = await startServer({ openTalk });
    const socket = await connect(port);

    const ready = nextJson(socket);
    socket.send(JSON.stringify(hello(7)));

    await expect(ready).resolves.toEqual({ type: "ready", version: 1, generation: 7 });
    expect(openTalk).toHaveBeenCalledOnce();
    expect(openTalk).toHaveBeenCalledWith(
      expect.objectContaining({ deviceId: "kitchen-xvf3800", generation: 7 }),
      expect.objectContaining({ audio: expect.any(Function), clear: expect.any(Function) }),
    );
  });

  it("closes the older generation before admitting a newer same-device session", async () => {
    const firstTalk = fakeTalk();
    const secondTalk = fakeTalk();
    const openTalk = vi
      .fn<BridgeDependencies["openTalk"]>()
      .mockResolvedValueOnce(firstTalk)
      .mockResolvedValueOnce(secondTalk);
    const { port } = await startServer({ openTalk });
    const first = await connect(port);
    first.send(JSON.stringify(hello(1)));
    await nextJson(first);

    const firstClosed = nextClose(first);
    const second = await connect(port);
    const secondReady = nextJson(second);
    second.send(JSON.stringify(hello(2)));

    await expect(firstClosed).resolves.toBe(1000);
    await expect(secondReady).resolves.toEqual({ type: "ready", version: 1, generation: 2 });
    expect(firstTalk.close).toHaveBeenCalledOnce();
    expect(openTalk).toHaveBeenCalledTimes(2);
  });

  it("forwards only current-generation audio and controls", async () => {
    const talk = fakeTalk();
    const { port } = await startServer({ openTalk: vi.fn(async () => talk) });
    const socket = await connect(port);
    socket.send(JSON.stringify(hello(4)));
    await nextJson(socket);

    socket.send(Buffer.alloc(960));
    socket.send(JSON.stringify({ type: "barge_in", version: 1, generation: 3 }));
    socket.send(JSON.stringify({ type: "barge_in", version: 1, generation: 4 }));
    socket.send(JSON.stringify({ type: "stop", version: 1, generation: 3 }));
    await vi.waitFor(() => expect(talk.appendAudio).toHaveBeenCalledOnce());
    await vi.waitFor(() => expect(talk.cancelOutput).toHaveBeenCalledOnce());

    expect(talk.appendAudio).toHaveBeenCalledWith(expect.any(Buffer));
    expect(talk.cancelOutput).toHaveBeenCalledWith("barge-in");
    expect(talk.close).not.toHaveBeenCalled();
  });

  it("does not dispatch queued old audio or controls after concurrent supersession", async () => {
    const firstAppend = deferred();
    const firstTalk = fakeTalk();
    vi.mocked(firstTalk.appendAudio).mockImplementationOnce(() => firstAppend.promise);
    const secondTalk = fakeTalk();
    const openTalk = vi
      .fn<BridgeDependencies["openTalk"]>()
      .mockResolvedValueOnce(firstTalk)
      .mockResolvedValueOnce(secondTalk);
    const { port } = await startServer({ openTalk });
    const first = await connect(port);
    first.send(JSON.stringify(hello(1)));
    await nextJson(first);

    first.send(Buffer.alloc(960, 1));
    first.send(Buffer.alloc(960, 2));
    first.send(JSON.stringify({ type: "barge_in", version: 1, generation: 1 }));
    await vi.waitFor(() => expect(firstTalk.appendAudio).toHaveBeenCalledOnce());

    const second = await connect(port);
    const secondReady = nextJson(second);
    second.send(JSON.stringify(hello(2)));
    await vi.waitFor(() => expect(firstTalk.close).toHaveBeenCalledOnce());
    firstAppend.resolve();

    await expect(secondReady).resolves.toEqual({ type: "ready", version: 1, generation: 2 });
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(firstTalk.appendAudio).toHaveBeenCalledOnce();
    expect(firstTalk.cancelOutput).not.toHaveBeenCalled();
  });

  it("does not dispatch messages queued behind stop", async () => {
    const talk = fakeTalk();
    const { port } = await startServer({ openTalk: vi.fn(async () => talk) });
    const socket = await connect(port);
    socket.send(JSON.stringify(hello(5)));
    await nextJson(socket);

    socket.send(JSON.stringify({ type: "stop", version: 1, generation: 5 }));
    socket.send(Buffer.alloc(960));
    socket.send(JSON.stringify({ type: "barge_in", version: 1, generation: 5 }));

    await vi.waitFor(() => expect(talk.close).toHaveBeenCalledOnce());
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(talk.appendAudio).not.toHaveBeenCalled();
    expect(talk.cancelOutput).not.toHaveBeenCalled();
  });

  it("closes on malformed post-authentication controls without leaking adapter errors", async () => {
    const talk = fakeTalk();
    const { port } = await startServer({ openTalk: vi.fn(async () => talk) });
    const socket = await connect(port);
    socket.send(JSON.stringify(hello(3)));
    await nextJson(socket);

    const closed = nextClose(socket);
    socket.send(JSON.stringify({ type: "pong", version: 2, generation: 3 }));

    await expect(closed).resolves.toBe(1011);
    await vi.waitFor(() => expect(talk.close).toHaveBeenCalledOnce());
  });

  it("enforces the 4096-byte WebSocket payload limit", async () => {
    const talk = fakeTalk();
    const { port } = await startServer({ openTalk: vi.fn(async () => talk) });
    const socket = await connect(port);
    socket.send(JSON.stringify(hello(3)));
    await nextJson(socket);

    const closed = nextClose(socket);
    socket.send(Buffer.alloc(4_097));

    await expect(closed).resolves.toBe(1009);
    await vi.waitFor(() => expect(talk.close).toHaveBeenCalledOnce());
    expect(talk.appendAudio).not.toHaveBeenCalled();
  });

  it("rejects a stale concurrent hello without disturbing the current owner", async () => {
    const talk = fakeTalk();
    const openTalk = vi.fn(async () => talk);
    const { port } = await startServer({ openTalk });
    const owner = await connect(port);
    owner.send(JSON.stringify(hello(8)));
    await nextJson(owner);

    const stale = await connect(port);
    const staleClosed = nextClose(stale);
    stale.send(JSON.stringify(hello(7)));

    await expect(staleClosed).resolves.toBe(1008);
    expect(talk.close).not.toHaveBeenCalled();
    expect(openTalk).toHaveBeenCalledOnce();
    const response = await fetch(`http://127.0.0.1:${port}/healthz`);
    await expect(response.json()).resolves.toEqual({ activeSessions: 1 });
  });

  it("reports only low-cardinality counts from /healthz", async () => {
    const { port } = await startServer();
    const socket = await connect(port);
    socket.send(JSON.stringify(hello(1)));
    await nextJson(socket);

    const response = await fetch(`http://127.0.0.1:${port}/healthz`);
    const body = (await response.json()) as Record<string, unknown>;

    expect(response.status).toBe(200);
    expect(body).toEqual({ activeSessions: 1 });
    expect(JSON.stringify(body)).not.toContain("kitchen");
    expect(JSON.stringify(body)).not.toContain(configuredCredential);
  });

  it("closes active Talk sessions during graceful server shutdown", async () => {
    const talk = fakeTalk();
    const { server, port } = await startServer({ openTalk: vi.fn(async () => talk) });
    const socket = await connect(port);
    socket.send(JSON.stringify(hello(1)));
    await nextJson(socket);

    const socketClosed = nextClose(socket);
    const serverClosed = new Promise<void>((resolve) => server.close(() => resolve()));

    await expect(socketClosed).resolves.toBe(1001);
    await serverClosed;
    expect(talk.close).toHaveBeenCalledOnce();
  });

  it("bounds shutdown and terminates a peer that withholds the close handshake", async () => {
    const { server, port } = await startServer({ shutdownGraceMs: 20 });
    const peer = connectTcp(port, "127.0.0.1");
    peer.write(
      "GET / HTTP/1.1\r\n" +
        `Host: 127.0.0.1:${port}\r\n` +
        "Upgrade: websocket\r\n" +
        "Connection: Upgrade\r\n" +
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
        "Sec-WebSocket-Version: 13\r\n\r\n",
    );
    await once(peer, "data");
    peer.pause();

    const started = Date.now();
    await new Promise<void>((resolve, reject) => {
      server.close((error) => (error === undefined ? resolve() : reject(error)));
    });

    expect(Date.now() - started).toBeLessThan(500);
    peer.destroy();
  });

  it("sanitizes rejecting adapter close without an unhandled rejection", async () => {
    const talk = fakeTalk();
    vi.mocked(talk.close).mockRejectedValue(new Error("sensitive adapter detail"));
    const onUnhandled = vi.fn();
    process.once("unhandledRejection", onUnhandled);
    const { port } = await startServer({ openTalk: vi.fn(async () => talk) });
    const socket = await connect(port);
    socket.send(JSON.stringify(hello(1)));
    await nextJson(socket);

    socket.close();
    await once(socket, "close");
    await new Promise((resolve) => setTimeout(resolve, 20));

    process.removeListener("unhandledRejection", onUnhandled);
    expect(onUnhandled).not.toHaveBeenCalled();
    expect(talk.close).toHaveBeenCalledOnce();
  });

  it("closes unauthenticated sockets during graceful server shutdown", async () => {
    const { server, port } = await startServer();
    const socket = await connect(port);
    const socketClosed = nextClose(socket);
    void new Promise<void>((resolve) => server.close(() => resolve()));

    const outcome = await Promise.race([
      socketClosed,
      new Promise<"timeout">((resolve) => setTimeout(() => resolve("timeout"), 100)),
    ]);

    expect(outcome).toBe(1001);
  });
});

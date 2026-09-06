import { describe, expect, it } from "vitest";

import { parseControl, validatePcmFrame } from "../src/protocol.js";

const validHello = {
  type: "hello",
  version: 1,
  generation: 7,
  deviceId: "kitchen-xvf3800",
  credential: "masked-test-value",
  sampleRate: 24_000,
  channels: 1,
  sampleFormat: "s16le",
  frameMs: 20,
  idleTimeoutSeconds: 30,
  firmware: "test",
} as const;

describe("parseControl", () => {
  it("accepts a strict version-1 hello", () => {
    expect(parseControl(JSON.stringify(validHello))).toMatchObject({
      type: "hello",
      version: 1,
      generation: 7,
    });
  });

  it("rejects unknown controls", () => {
    expect(() =>
      parseControl(JSON.stringify({ type: "unknown", version: 1, generation: 7 })),
    ).toThrow();
  });

  it("rejects extra fields", () => {
    expect(() =>
      parseControl(JSON.stringify({ ...validHello, unexpected: true })),
    ).toThrow();
  });

  it.each([4, 301])("rejects idle timeout %s outside 5..300", (idleTimeoutSeconds) => {
    expect(() =>
      parseControl(JSON.stringify({ ...validHello, idleTimeoutSeconds })),
    ).toThrow();
  });
});

describe("validatePcmFrame", () => {
  it("accepts an exact 20 ms PCM frame", () => {
    const frame = Buffer.alloc(960);
    expect(validatePcmFrame(frame)).toBe(frame);
  });

  it("rejects odd-length binary frames", () => {
    expect(() => validatePcmFrame(Buffer.alloc(959))).toThrow();
  });

  it("rejects binary frames larger than 3840 bytes", () => {
    expect(() => validatePcmFrame(Buffer.alloc(3_842))).toThrow();
  });
});

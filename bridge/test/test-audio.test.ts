import { describe, expect, it } from "vitest";

import { makeTonePcm } from "../src/test-audio.js";

describe("makeTonePcm", () => {
  it("makes a deterministic 24 kHz mono PCM16 tone", () => {
    const pcm = makeTonePcm({ frequencyHz: 440, durationMs: 20, amplitude: 0.5 });

    expect(pcm).toHaveLength(960);
    expect(pcm.readInt16LE(0)).toBe(0);
    expect(pcm.readInt16LE(2)).toBe(1883);
    expect(pcm.readInt16LE(4)).toBe(3741);
    expect(pcm.readInt16LE(6)).toBe(5550);
  });

  it("rejects invalid tone parameters", () => {
    expect(() => makeTonePcm({ frequencyHz: 0, durationMs: 20, amplitude: 0.5 })).toThrow("frequencyHz");
    expect(() => makeTonePcm({ frequencyHz: 440, durationMs: 0, amplitude: 0.5 })).toThrow("durationMs");
    expect(() => makeTonePcm({ frequencyHz: 440, durationMs: 20, amplitude: 1.1 })).toThrow("amplitude");
  });
});

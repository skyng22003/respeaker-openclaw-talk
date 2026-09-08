const SAMPLE_RATE_HZ = 24_000;

export interface ToneOptions {
  frequencyHz: number;
  durationMs: number;
  amplitude: number;
}

export function makeTonePcm({ frequencyHz, durationMs, amplitude }: ToneOptions): Buffer {
  if (!Number.isFinite(frequencyHz) || frequencyHz <= 0 || frequencyHz >= SAMPLE_RATE_HZ / 2) {
    throw new RangeError("frequencyHz must be between 0 and the Nyquist frequency");
  }
  if (!Number.isInteger(durationMs) || durationMs <= 0) {
    throw new RangeError("durationMs must be a positive integer");
  }
  if (!Number.isFinite(amplitude) || amplitude < 0 || amplitude > 1) {
    throw new RangeError("amplitude must be between 0 and 1");
  }

  const sampleCount = (SAMPLE_RATE_HZ * durationMs) / 1_000;
  if (!Number.isInteger(sampleCount)) {
    throw new RangeError("durationMs must produce a whole number of samples");
  }

  const pcm = Buffer.alloc(sampleCount * 2);
  for (let index = 0; index < sampleCount; index++) {
    const sample = Math.round(Math.sin((2 * Math.PI * frequencyHz * index) / SAMPLE_RATE_HZ) * amplitude * 32_767);
    pcm.writeInt16LE(sample, index * 2);
  }
  return pcm;
}

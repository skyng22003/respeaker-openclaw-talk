import { z } from "zod";

const sessionFields = {
  version: z.literal(1),
  generation: z.number().int().nonnegative().safe(),
} as const;

const helloSchema = z
  .object({
    type: z.literal("hello"),
    ...sessionFields,
    deviceId: z.string().min(1).max(128),
    credential: z.string().min(1).max(512),
    sampleRate: z.literal(24_000),
    channels: z.literal(1),
    sampleFormat: z.literal("s16le"),
    frameMs: z.literal(20),
    idleTimeoutSeconds: z.number().int().min(5).max(300),
    firmware: z.string().min(1).max(128),
  })
  .strict();

const bargeInSchema = z
  .object({ type: z.literal("barge_in"), ...sessionFields })
  .strict();
const stopSchema = z
  .object({ type: z.literal("stop"), ...sessionFields })
  .strict();
const pongSchema = z
  .object({ type: z.literal("pong"), ...sessionFields })
  .strict();

const deviceControlSchema = z.discriminatedUnion("type", [
  helloSchema,
  bargeInSchema,
  stopSchema,
  pongSchema,
]);

const readySchema = z
  .object({ type: z.literal("ready"), ...sessionFields })
  .strict();
const activitySchema = z
  .object({
    type: z.literal("activity"),
    ...sessionFields,
    activity: z.enum(["listening", "thinking", "speaking"]),
  })
  .strict();
const clearSchema = z
  .object({ type: z.literal("clear"), ...sessionFields })
  .strict();
const errorSchema = z
  .object({
    type: z.literal("error"),
    ...sessionFields,
    code: z.string().min(1).max(64),
    message: z.string().min(1).max(256).optional(),
  })
  .strict();
const closeSchema = z
  .object({
    type: z.literal("close"),
    ...sessionFields,
    reason: z.string().min(1).max(64),
  })
  .strict();
const pingSchema = z
  .object({ type: z.literal("ping"), ...sessionFields })
  .strict();

export const bridgeControlSchema = z.discriminatedUnion("type", [
  readySchema,
  activitySchema,
  clearSchema,
  errorSchema,
  closeSchema,
  pingSchema,
]);

export type DeviceHello = z.infer<typeof helloSchema>;
export type DeviceControl = z.infer<typeof deviceControlSchema>;
export type BridgeControl = z.infer<typeof bridgeControlSchema>;

export function parseControl(raw: string): DeviceControl {
  return deviceControlSchema.parse(JSON.parse(raw) as unknown);
}

export function validatePcmFrame(data: Buffer): Buffer {
  if (data.length === 0 || data.length > 3_840 || data.length % 2 !== 0) {
    throw new Error("PCM frame must contain 2..3840 bytes of aligned PCM16 audio");
  }
  return data;
}

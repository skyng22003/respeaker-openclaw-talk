import { z } from "zod";

const configSchema = z
  .object({
    host: z.string().min(1),
    port: z.coerce.number().int().min(1).max(65_535),
    credential: z.string().min(1),
    gatewayUrl: z.string().url(),
    gatewayToken: z.string().min(1),
    sessionKey: z.string().min(1),
  })
  .strict();

export interface BridgeConfig {
  host: string;
  port: number;
  credential: string;
  gatewayUrl: string;
  gatewayToken: string;
  sessionKey: string;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): BridgeConfig {
  return configSchema.parse({
    host: env.BRIDGE_HOST ?? "127.0.0.1",
    port: env.BRIDGE_PORT ?? "8787",
    credential: env.DEVICE_CREDENTIAL,
    gatewayUrl: env.GATEWAY_URL,
    gatewayToken: env.GATEWAY_TOKEN,
    sessionKey: env.TALK_SESSION_KEY ?? "main",
  });
}

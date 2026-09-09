import { describe, expect, it } from "vitest";

import { loadConfig } from "../src/config.js";

const baseEnv = {
  DEVICE_CREDENTIAL: "device-secret",
  GATEWAY_URL: "ws://127.0.0.1:18789",
  GATEWAY_DEVICE_TOKEN: "scoped-token",
  GATEWAY_DEVICE_ID: "device-id",
  GATEWAY_DEVICE_PUBLIC_KEY_FILE: "/run/secrets/gateway-device-public.pem",
  GATEWAY_DEVICE_PRIVATE_KEY_FILE: "/run/secrets/gateway-device-private.pem",
};

describe("bridge configuration", () => {
  it("loads the paired Gateway device credential paths", () => {
    expect(loadConfig(baseEnv)).toMatchObject({
      gatewayToken: "scoped-token",
      gatewayDeviceId: "device-id",
      gatewayDevicePublicKeyFile: "/run/secrets/gateway-device-public.pem",
      gatewayDevicePrivateKeyFile: "/run/secrets/gateway-device-private.pem",
    });
  });

  it("rejects the legacy shared Gateway token configuration", () => {
    expect(() => loadConfig({ ...baseEnv, GATEWAY_DEVICE_TOKEN: undefined, GATEWAY_TOKEN: "shared-token" })).toThrow();
  });
});

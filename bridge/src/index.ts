import { loadConfig } from "./config.js";
import { createBridgeServer } from "./server.js";
import { openGatewayTalk } from "./gateway-talk.js";
import { readFileSync } from "node:fs";

const config = loadConfig();
const server = createBridgeServer({
  credential: config.credential,
  openTalk: async (device, callbacks) => {
    return await openGatewayTalk({
      url: config.gatewayUrl,
      token: config.gatewayToken,
      ...(config.gatewayBootstrapToken === undefined ? {} : { bootstrapToken: config.gatewayBootstrapToken }),
      sessionKey: config.sessionKey,
      deviceIdentity: {
        deviceId: config.gatewayDeviceId,
        publicKeyPem: readFileSync(config.gatewayDevicePublicKeyFile, "utf8"),
        privateKeyPem: readFileSync(config.gatewayDevicePrivateKeyFile, "utf8"),
      },
    }, device, callbacks);
  },
});

server.listen(config.port, config.host, () => {
  console.log(`reSpeaker Talk bridge listening on ${config.host}:${config.port}`);
});

let stopping = false;
function stop(): void {
  if (stopping) return;
  stopping = true;
  server.close((error) => {
    if (error !== undefined) {
      console.error("Bridge shutdown failed");
      process.exitCode = 1;
    }
  });
}

process.once("SIGINT", stop);
process.once("SIGTERM", stop);

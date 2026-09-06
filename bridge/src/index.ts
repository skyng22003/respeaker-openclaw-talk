import { loadConfig } from "./config.js";
import { createBridgeServer } from "./server.js";

const config = loadConfig();
const server = createBridgeServer({
  credential: config.credential,
  openTalk: async () => {
    throw new Error("OpenClaw Talk adapter is not configured in bridge core");
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

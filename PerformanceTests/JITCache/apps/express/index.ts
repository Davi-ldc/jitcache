// Bun's bench/express/express.mjs on a port the system picks. The module waits until the server listens, so startup
// ends there, and fetch sends each request through Bun's fetch to that port, as a platform that runs an HTTP server
// forwards a request to it. The server does not keep the process alive, so the process ends after its last call.
import express from "express";
import type { Server } from "node:http";
import type { AddressInfo } from "node:net";

const app = express();
let i = 0;

app.get("/", (req, res) => {
  res.send("Hello World! (request number: " + i++ + ")");
});

const server = await new Promise<Server>((resolve, reject) => {
  // Loopback only, so nothing from outside the host reaches a measured process.
  const server = app.listen(0, "127.0.0.1", (error?: Error) => {
    if (error) return reject(error);
    console.log(`Express server listening on port ${(server.address() as AddressInfo).port}`);
    resolve(server);
  });
});
server.unref();
const origin = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;

export default {
  fetch(request: Request) {
    const { pathname, search } = new URL(request.url);
    return fetch(`${origin}${pathname}${search}`, { method: request.method, headers: request.headers, body: request.body });
  },
};

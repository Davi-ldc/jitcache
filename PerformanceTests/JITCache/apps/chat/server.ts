// Bun's bench/websocket-server/chat-server.bun.js on a port the system picks, since its clients connect from the same
// process. Upstream's server publishes `ready` 100 ms after the last client connects, which tells its client process to
// start; here the clients start with the first call, so the server only logs that every client connected.
export const CLIENTS_TO_WAIT_FOR = 32;
var remainingClients = CLIENTS_TO_WAIT_FOR;

export const server = Bun.serve<{ name: string }>({
  // Loopback only, so no client from outside the host joins the room.
  hostname: "127.0.0.1",
  port: 0,
  websocket: {
    open(ws) {
      ws.subscribe("room");

      remainingClients--;
      console.log(`${ws.data.name} connected (${remainingClients} remain)`);

      if (remainingClients === 0) {
        console.log("All clients connected");
      }
    },
    message(ws, msg) {
      const out = `${ws.data.name}: ${msg}`;
      // publishText returns -1 when the message is queued behind backpressure, 0 when it is dropped.
      if (ws.publishText("room", out) === 0) {
        throw new Error("Failed to publish message");
      }
    },
    close(ws) {
      remainingClients++;
    },

    perMessageDeflate: false,
    publishToSelf: true,
  },

  fetch(req, server) {
    if (
      server.upgrade(req, {
        data: {
          name: new URL(req.url).searchParams.get("name") || "Client #" + (CLIENTS_TO_WAIT_FOR - remainingClients),
        },
      })
    )
      return;

    return new Response("Error");
  },
});

console.log(`Waiting for ${remainingClients} clients to connect...\n`, `  http://${server.hostname}:${server.port}/`);

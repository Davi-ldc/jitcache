// The chat room of Bun's websocket-server bench: the server of server.ts and the clients of upstream's chat-client.mjs,
// with its names, which connect while the module loads, so startup includes their upgrades. A call is one of upstream's
// rounds at a smaller size: every client sends the request's messages, a JSON list, and the call ends when every client
// has received every client's messages back from the room. Upstream's client sends its 50-message list 32 times per
// round; requests/chat.json sends it once, so a round is 1600 messages in and 51,200 out.
//
// The clients are TCP sockets that speak WebSocket themselves. Each sends a round's messages as one write of masked
// frames, built once per list, and counts the bytes it receives, which the list fixes in advance. They thus run one
// callback per socket and round, where a WebSocket client would run one event per message. Their writes take about
// 1.5% of a round; their reads, a new buffer for each of the 1.3 MB of echoes, run on the same CPU too, alike with and
// without JITCache, so they lower the ratio without biasing it. The call answers with the bytes the first client received: the server and the clients share one
// event loop, which reads the server's sockets in the order the clients wrote to them, so every process gets the same
// bytes.
import type { Socket } from "bun";
import { CLIENTS_TO_WAIT_FOR, server } from "./server.ts";

// The process ends after its last call, as with the other cases.
server.unref();

const NAMES = Array.from({ length: 50 }, (a, i) => [
  "Alice" + i,
  "Bob" + i,
  "Charlie" + i,
  "David" + i,
  "Eve" + i,
  "Frank" + i,
  "Grace" + i,
  "Heidi" + i,
  "Ivan" + i,
  "Judy" + i,
  "Karl" + i,
  "Linda" + i,
  "Mike" + i,
  "Nancy" + i,
  "Oscar" + i,
  "Peggy" + i,
  "Quentin" + i,
  "Ruth" + i,
  "Steve" + i,
  "Trudy" + i,
  "Ursula" + i,
  "Victor" + i,
  "Wendy" + i,
  "Xavier" + i,
  "Yvonne" + i,
  "Zach" + i,
])
  .flat()
  .slice(0, CLIENTS_TO_WAIT_FOR);

interface Client {
  name: string;
  socket: Socket;
  received: number; // bytes, in the current round
}

// The round under way: the bytes each client is due, the clients still short of them, and what the first client got.
let round: { expected: number; remaining: number; firstBytes: Uint8Array[]; resolve: () => void; reject: (error: Error) => void } | null = null;
// A connection that closed or failed, which no later round can complete without.
let broken: Error | null = null;

function fail(error: Error) {
  broken ??= error;
  round?.reject(error);
}

// The server's frames carry no mask: a 2-byte header, or 4 bytes with a 16-bit length, before the payload.
const serverFrameBytes = (payloadBytes: number) => (payloadBytes < 126 ? 2 : 4) + payloadBytes;

// A client's text frame, masked as the protocol requires of clients, with a zero key, which leaves the payload as is.
function clientFrame(payload: Uint8Array): Uint8Array {
  const header = payload.length < 126 ? [0x81, 0x80 | payload.length] : [0x81, 0x80 | 126, payload.length >> 8, payload.length & 0xff];
  const frame = new Uint8Array(header.length + 4 + payload.length);
  frame.set(header);
  frame.set(payload, header.length + 4);
  return frame;
}

function connect(name: string, index: number): Promise<Client> {
  return new Promise((resolve, reject) => {
    let handshake: string | null = "";
    Bun.connect({
      hostname: "127.0.0.1",
      port: server.port!,
      socket: {
        open(socket) {
          socket.write(`GET /?name=${name} HTTP/1.1\r\nHost: 127.0.0.1:${server.port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n`);
        },
        data(socket, data) {
          if (handshake !== null) {
            handshake += data.toString("latin1");
            const end = handshake.indexOf("\r\n\r\n");
            if (end < 0) return;
            if (!handshake.startsWith("HTTP/1.1 101 ") || end + 4 !== handshake.length) return reject(new Error(`${name} got ${JSON.stringify(handshake)} for its upgrade`));
            handshake = null;
            socket.unref();
            return resolve({ name, socket, received: 0 });
          }
          const current = round;
          if (!current) return fail(new Error(`${name} received ${data.length} bytes between rounds`));
          const me = clients[index];
          me.received += data.length;
          if (index === 0) current.firstBytes.push(new Uint8Array(data));
          if (me.received > current.expected) current.reject(new Error(`${name} received ${me.received} bytes in a round that sends it ${current.expected}`));
          else if (me.received === current.expected && --current.remaining === 0) current.resolve();
        },
        close() {
          fail(new Error(`${name}'s connection closed`));
        },
        error(socket, error) {
          fail(error);
        },
      },
    }).catch(reject);
  });
}

const clients = await Promise.all(NAMES.map(connect));

// By request body, a list of messages: the frames each client sends in a round, and the bytes each client receives.
const lists = new Map<string, { frames: Uint8Array; expected: number }>();

function listOf(text: string) {
  let list = lists.get(text);
  if (!list) {
    const messages: string[] = JSON.parse(text);
    const encoder = new TextEncoder();
    const frames = messages.map(message => clientFrame(encoder.encode(message)));
    const bytes = new Uint8Array(frames.reduce((total, frame) => total + frame.length, 0));
    let offset = 0;
    for (const frame of frames) {
      bytes.set(frame, offset);
      offset += frame.length;
    }
    let expected = 0;
    for (const name of NAMES) for (const message of messages) expected += serverFrameBytes(encoder.encode(`${name}: ${message}`).length);
    lists.set(text, (list = { frames: bytes, expected }));
  }
  return list;
}

export default {
  async fetch(request: Request) {
    if (broken) throw broken;
    const { frames, expected } = listOf(await request.text());
    const firstBytes: Uint8Array[] = [];
    const done = new Promise<void>((resolve, reject) => {
      round = { expected, remaining: clients.length, firstBytes, resolve, reject };
    });
    try {
      for (const client of clients) {
        client.received = 0;
        const written = client.socket.write(frames);
        if (written !== frames.length) throw new Error(`${client.name} wrote ${written} of its ${frames.length} bytes`);
      }
      await done;
    } finally {
      round = null;
    }
    return new Response(new Blob(firstBytes));
  },
};

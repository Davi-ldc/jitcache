// The process a macro bench measures: NODE_ENV=production bun run.ts <case> [--passes=N]. It imports the case's app from
// apps/ and the requests of one pass from requests/, then writes JSON lines to standard output, which
// Tools/Scripts/jitcache-macro-bench.ts reads: a ready line, one line per pass with its wall time and the checksum of
// every response, and an end line. A pass calls the app's fetch handler with each request in order, as a serverless
// platform invokes a function.

const cases: Record<string, { app: string; requests: string }> = {
  hono: { app: "./apps/hono/index.ts", requests: "./requests/hono.json" },
  elysia: { app: "./apps/elysia/index.ts", requests: "./requests/elysia.json" },
  "react-ssr": { app: "./apps/react-ssr/index.tsx", requests: "./requests/react-ssr.json" },
  "preact-ssr": { app: "./apps/preact-ssr/index.tsx", requests: "./requests/preact-ssr.json" },
  pdf: { app: "./apps/pdf/index.ts", requests: "./requests/pdf.ts" },
  webhook: { app: "./apps/webhook/index.ts", requests: "./requests/webhook.json" },
};

// One request of a pass, as the requests files write it. A body that is neither a string nor bytes is sent as JSON.
// `status` is the status the app must answer. `save` keeps the response's JSON under a name, and a later request of the
// same pass can put one of its fields into its path or a header as {name.field}, as a client sends back the id or the
// token an API gave it.
interface BenchRequest {
  method: string;
  path: string;
  headers?: Record<string, string>;
  body?: unknown;
  status: number;
  save?: string;
}

interface App {
  fetch(request: Request): Response | Promise<Response>;
}

interface Answer {
  status: number;
  headers: Headers;
  body: Uint8Array;
}

const defaultPasses = 34;
const usage = `Usage: NODE_ENV=production bun run.ts <${Object.keys(cases).join("|")}> [--passes=N]`;

function commandLine(argv: string[]): { name: string; passes: number } | string {
  const [name, ...options] = argv;
  if (!name || !Object.hasOwn(cases, name)) return name ? `no case named ${name}` : "no case given";
  let passes = defaultPasses;
  for (const option of options) {
    const match = /^--passes=(.*)$/.exec(option);
    if (!match) return `unknown option ${option}`;
    passes = Number(match[1]);
    if (!/^\d+$/.test(match[1]) || passes < 1) return `--passes takes a positive integer, not ${match[1]}`;
  }
  // A deployment runs with NODE_ENV=production. React reads it when it loads, and Bun's JSX transform reads it when the
  // process starts, so a script cannot set it for itself: a hand run without it would measure React's development build.
  if (process.env.NODE_ENV !== "production") return "NODE_ENV must be production, as in a deployment";
  return { name, passes };
}

// Everything that does not depend on an earlier response is prepared once, before the ready line.
function prepare(request: BenchRequest) {
  const headers: Record<string, string> = { ...request.headers };
  let body = request.body as BodyInit | undefined;
  if (body !== undefined && typeof body !== "string" && !(body instanceof Uint8Array)) {
    body = JSON.stringify(body);
    headers["content-type"] ??= "application/json";
  }
  return { ...request, headers: Object.entries(headers), body };
}

async function runPass(app: App, requests: ReturnType<typeof prepare>[]): Promise<Answer[]> {
  const saved = new Map<string, Record<string, unknown>>();
  const fill = (text: string) => text.replace(/\{(\w+)\.(\w+)\}/g, (_, name: string, field: string) => String(saved.get(name)?.[field]));
  const answers: Answer[] = [];
  for (const request of requests) {
    const path = fill(request.path);
    const headers = request.headers.map(([name, value]): [string, string] => [name, fill(value)]);
    const response = await app.fetch(new Request(`http://localhost${path}`, { method: request.method, headers, body: request.body }));
    const body = await response.bytes();
    if (response.status !== request.status) {
      throw new Error(`${request.method} ${path} answered ${response.status} where ${request.status} was due: ${new TextDecoder().decode(body).slice(0, 500)}`);
    }
    if (request.save) saved.set(request.save, JSON.parse(new TextDecoder().decode(body)));
    answers.push({ status: response.status, headers: response.headers, body });
  }
  return answers;
}

// SHA-256 of every answer's status, headers and body in order, each body after its length. No app sets a header that
// holds a time or a random id, so every header counts.
function checksum(answers: Answer[]): string {
  const hasher = new Bun.CryptoHasher("sha256");
  for (const { status, headers, body } of answers) {
    let head = `${status}\n`;
    for (const [name, value] of headers) head += `${name}: ${value}\n`;
    hasher.update(`${head}${body.byteLength}:`);
    hasher.update(body);
  }
  return hasher.digest("hex");
}

const parsed = commandLine(process.argv.slice(2));
if (typeof parsed === "string") {
  console.error(`run.ts: ${parsed}\n${usage}`);
  process.exit(2);
}

try {
  const paths = cases[parsed.name];
  const app: App = (await import(paths.app)).default;
  const requests = ((await import(paths.requests)).default as BenchRequest[]).map(prepare);
  console.log(JSON.stringify({ event: "ready", startupNanoseconds: Bun.nanoseconds() }));
  for (let pass = 1; pass <= parsed.passes; ++pass) {
    const start = performance.now();
    const answers = await runPass(app, requests);
    const milliseconds = performance.now() - start;
    // Hashing comes after the clock stops, so the pass's time holds only the requests.
    console.log(JSON.stringify({ event: "pass", pass, milliseconds, checksum: checksum(answers) }));
  }
  console.log(JSON.stringify({ event: "end", rssBytes: process.memoryUsage().rss }));
  // The process then ends when its event loop is empty, where Bun's exit hook runs JITCache's delta in a producer.
} catch (error) {
  console.error(`run.ts: ${parsed.name}: ${error instanceof Error ? (error.stack ?? error.message) : String(error)}`);
  process.exit(1);
}

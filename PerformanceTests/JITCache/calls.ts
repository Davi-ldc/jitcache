// The calls a macro bench process makes (docs/JitCache/HARNESS.md, Benches), whether it runs from source through run.ts
// or as an app bench.ts built with this module inside. Once the app and its request variants are loaded, the process
// writes its ready line, makes N calls, each sending one request to the app's fetch handler, as a serverless platform
// invokes a function, and writes its end line. In the fixed branch every call sends variant 1; in the varied branch
// call n sends variant n, starting over at variant 1 after the last. N may be 0, and the process then ends right after
// its ready line.
//
//   --calls=N --branch=fixed|varied [--kill-at-end] [--mark-calls]
//
// Standard output carries JSON lines for bench.ts, the driver: a ready line, one line per call and an end line. The ready
// and end lines carry Bun.nanoseconds(), Bun's clock since its VirtualMachine was created, so the driver can tell startup
// and the exit from the wall time it measures. Each line goes out through a synchronous write to fd 1, so a process that
// ends or kills itself right after a line has written all of it. --mark-calls writes `call <n>` to standard error right
// before call n; JSC's compile log goes there too, so the log splits by call. --kill-at-end sends the process SIGKILL
// right after its end line, so Bun's exit hook, which runs JITCache's delta in a producer, never runs.

import { readdirSync, readFileSync, writeSync } from "node:fs";

// One request variant, as the requests files write it. A body that is neither a string nor bytes is sent as JSON.
// `status` is the status the app must answer. `save` keeps the response's JSON under a name for the rest of the process,
// and a later call can put one of its fields into its path or a header as {name.field}, as a client sends back the id or
// the token an API gave it. Variant 1 uses no saved field, since the fixed branch sends it alone.
export interface Variant {
  method: string;
  path: string;
  headers?: Record<string, string>;
  body?: unknown;
  status: number;
  save?: string;
}

export interface App {
  fetch(request: Request): Response | Promise<Response>;
}

type Branch = "fixed" | "varied";

// What the arguments ask for.
export interface Plan {
  calls: number;
  branch: Branch;
  killAtEnd: boolean;
  markCalls: boolean;
}

export const planUsage = "--calls=N --branch=fixed|varied [--kill-at-end] [--mark-calls]";

// The flags readPlan reads, by which a built app picks its arguments out of process.argv.
const planFlags = ["--calls", "--branch", "--kill-at-end", "--mark-calls"];

// The plan, or what keeps the arguments from giving one.
export function readPlan(options: string[]): Plan | string {
  let calls: number | null = null;
  let branch: Branch | null = null;
  let killAtEnd = false;
  let markCalls = false;
  for (const option of options) {
    const equals = option.indexOf("=");
    const flag = equals < 0 ? option : option.slice(0, equals);
    const value = equals < 0 ? null : option.slice(equals + 1);
    if (flag === "--calls" && value !== null && /^\d+$/.test(value)) calls = Number(value);
    else if (flag === "--branch" && (value === "fixed" || value === "varied")) branch = value;
    else if (flag === "--kill-at-end" && value === null) killAtEnd = true;
    else if (flag === "--mark-calls" && value === null) markCalls = true;
    else return `cannot read ${option}`;
  }
  if (calls === null || branch === null) return "--calls and --branch are required";
  // A deployment runs with NODE_ENV=production. React reads it when it loads, and Bun's JSX transform reads it when the
  // process starts, so a script cannot set it for itself: a hand run without it would measure React's development build.
  // In a built app the bundler has replaced process.env.NODE_ENV with the value the build saw, so there the check
  // refuses an app built without it.
  if (process.env.NODE_ENV !== "production") return "NODE_ENV must be production, as in a deployment";
  return { calls, branch, killAtEnd, markCalls };
}

// Writes all of text to fd before it returns.
export function emit(fd: number, text: string) {
  const bytes = new TextEncoder().encode(text);
  for (let written = 0; written < bytes.length; ) written += writeSync(fd, bytes, written);
}

const emitLine = (event: object) => emit(1, `${JSON.stringify(event)}\n`);

// Everything that does not depend on an earlier response is prepared once, before the ready line.
export function prepare(variant: Variant) {
  const headers: Record<string, string> = { ...variant.headers };
  let body = variant.body as BodyInit | undefined;
  if (body !== undefined && typeof body !== "string" && !(body instanceof Uint8Array)) {
    body = JSON.stringify(body);
    headers["content-type"] ??= "application/json";
  }
  return { ...variant, headers: Object.entries(headers), body };
}

// SHA-256 of the response's status, headers and body, the body after its length. No app sets a header that holds a time
// or a random id, so every header counts.
function checksum(response: Response, body: Uint8Array): string {
  const hasher = new Bun.CryptoHasher("sha256");
  let head = `${response.status}\n`;
  for (const [name, value] of response.headers) head += `${name}: ${value}\n`;
  hasher.update(`${head}${body.byteLength}:`);
  hasher.update(body);
  return hasher.digest("hex");
}

// The process's memory and what the driver checks about the process itself: the CPUs it may run on, the parallelism
// JavaScript sees, and its threads by name, among them JSC's JIT workers when concurrent JIT is on. The peak is VmHWM,
// which starts over at exec: getrusage's peak also holds the RSS of the process that spawned this one.
function endFacts() {
  const status = readFileSync("/proc/self/status", "utf8");
  const threads: Record<string, number> = {};
  for (const task of readdirSync("/proc/self/task")) {
    let name: string;
    try {
      name = readFileSync(`/proc/self/task/${task}/comm`, "utf8").trimEnd();
    } catch {
      continue; // the thread ended after the listing
    }
    threads[name] = (threads[name] ?? 0) + 1;
  }
  const peakKibibytes = /^VmHWM:\s*(\d+) kB$/m.exec(status)?.[1];
  return {
    rssBytes: process.memoryUsage().rss,
    peakRssBytes: peakKibibytes === undefined ? null : Number(peakKibibytes) * 1024,
    cpusAllowed: /^Cpus_allowed_list:\s*(\S+)$/m.exec(status)?.[1] ?? null,
    hardwareConcurrency: navigator.hardwareConcurrency,
    threads,
  };
}

// The ready line, the calls the plan asks for over the prepared variants, and the end line. A call that fails throws.
export async function makeCalls(app: App, variants: ReturnType<typeof prepare>[], plan: Plan) {
  emitLine({ event: "ready", startupNanoseconds: Bun.nanoseconds() });
  const saved = new Map<string, Record<string, unknown>>();
  for (let call = 1; call <= plan.calls; ++call) {
    const variant = plan.branch === "fixed" ? 1 : ((call - 1) % variants.length) + 1;
    const request = variants[variant - 1];
    const fill = (text: string) =>
      text.replace(/\{(\w+)\.(\w+)\}/g, (_, name: string, field: string) => {
        const value = saved.get(name)?.[field];
        if (value === undefined) throw new Error(`variant ${variant} uses {${name}.${field}}, which no earlier call saved`);
        return String(value);
      });
    const path = fill(request.path);
    const headers = request.headers.map(([name, value]): [string, string] => [name, fill(value)]);
    if (plan.markCalls) emit(2, `call ${call}\n`);
    const start = performance.now();
    const response = await app.fetch(new Request(`http://localhost${path}`, { method: request.method, headers, body: request.body }));
    const body = await response.bytes();
    const milliseconds = performance.now() - start;
    // The client's side of the call comes after the clock stops: the status check, the saved response and the checksum.
    if (response.status !== request.status) {
      throw new Error(`call ${call}, variant ${variant}: ${request.method} ${path} answered ${response.status} where ${request.status} was due: ${new TextDecoder().decode(body).slice(0, 500)}`);
    }
    if (request.save) saved.set(request.save, JSON.parse(new TextDecoder().decode(body)));
    emitLine({ event: "call", call, variant, milliseconds, checksum: checksum(response, body) });
  }
  // The clock is read last, after the end facts, so the driver's exit time starts at the end line's write.
  emitLine({ event: "end", ...endFacts(), endNanoseconds: Bun.nanoseconds() });
  if (plan.killAtEnd) process.kill(process.pid, "SIGKILL");
  // Otherwise the process ends when its event loop is empty, and Bun's exit hook then runs JITCache's delta in a producer.
}

// The process of an app bench.ts built, whose entry imported the app and its request variants. It picks its arguments
// out of process.argv by name, since a compiled executable's process.argv differs from that of `bun app.js`, and
// otherwise does what run.ts does.
export async function serveBuilt(name: string, app: App, requests: Variant[]) {
  const plan = readPlan(process.argv.filter(argument => planFlags.includes(argument.split("=")[0])));
  if (typeof plan === "string") {
    emit(2, `${name}: ${plan}\nUsage: NODE_ENV=production <bun app.js, or the executable> ${planUsage}\n`);
    process.exit(2);
  }
  try {
    const variants = requests.map(prepare);
    if (!variants.length) throw new Error(`${name}'s requests hold no request`);
    await makeCalls(app, variants, plan);
  } catch (error) {
    emit(2, `${name}: ${error instanceof Error ? (error.stack ?? error.message) : String(error)}\n`);
    process.exit(1);
  }
}

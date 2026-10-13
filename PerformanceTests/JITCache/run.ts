// The process a macro bench measures: NODE_ENV=production bun run.ts <case> [--passes=N]. It sets the case up, then
// writes JSON lines to standard output, which Tools/Scripts/jitcache-macro-bench.ts reads: a ready line once the case
// can serve, one line per pass with its wall time and the checksum of everything the pass produced, and an end line.

// A case's module exports setup(), which does everything a pass must not do: imports, reading the inputs, building data,
// opening a server. A pass runs every unit of the case once, in order, and returns what the units produced.
export interface Workload {
  pass(pass: number): Promise<string[]> | string[];
  close?(): Promise<void> | void;
}

// Each loader imports one case alone, so a process loads only the libraries its case uses.
const cases: Record<string, () => Promise<{ setup(): Promise<Workload> }>> = {
  api: () => import("./cases/api.ts"),
  ssr: () => import("./cases/ssr.tsx"),
  markdown: () => import("./cases/markdown.ts"),
  tsc: () => import("./cases/tsc.ts"),
};

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
  // A deployment runs with NODE_ENV=production. React reads it when it loads, and Bun's transform of the SSR case's JSX
  // reads it when the process starts, so a script cannot set it for itself: a hand run without it would measure React's
  // development build.
  if (process.env.NODE_ENV !== "production") return "NODE_ENV must be production, as in a deployment";
  return { name, passes };
}

// SHA-256 of the parts in order, each after its length, so two different splits of the same text differ.
function checksum(parts: string[]): string {
  const hasher = new Bun.CryptoHasher("sha256");
  for (const part of parts) {
    hasher.update(`${part.length}:`);
    hasher.update(part);
  }
  return hasher.digest("hex");
}

const parsed = commandLine(process.argv.slice(2));
if (typeof parsed === "string") {
  console.error(`run.ts: ${parsed}\n${usage}`);
  process.exit(2);
}

try {
  const workload = await (await cases[parsed.name]()).setup();
  console.log(JSON.stringify({ event: "ready", startupNanoseconds: Bun.nanoseconds() }));
  for (let pass = 1; pass <= parsed.passes; ++pass) {
    const start = performance.now();
    const output = await workload.pass(pass);
    const milliseconds = performance.now() - start;
    // Hashing comes after the clock stops, so the pass's time holds only the case's own work.
    console.log(JSON.stringify({ event: "pass", pass, milliseconds, checksum: checksum(output) }));
  }
  await workload.close?.();
  console.log(JSON.stringify({ event: "end", rssBytes: process.memoryUsage().rss }));
  // The process then ends when its event loop is empty, where Bun's exit hook runs JITCache's delta in a producer.
} catch (error) {
  console.error(`run.ts: ${parsed.name}: ${error instanceof Error ? (error.stack ?? error.message) : String(error)}`);
  // A failed case can leave its server listening, which would keep the event loop alive.
  process.exit(1);
}

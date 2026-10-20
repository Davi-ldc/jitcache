// The process a macro bench measures in its source variant (docs/JitCache/HARNESS.md, Benches):
//
//   NODE_ENV=production bun run.ts <case> --calls=N --branch=fixed|varied [--kill-at-end] [--mark-calls]
//
// It imports the case's app from apps/ and its request variants from requests/, through Bun's runtime transpiler, then
// makes the calls through calls.ts, which writes the lines bench.ts reads. The other variants run apps that bench.ts
// builds from the modules this table names, each with calls.ts inside, so every variant makes the same calls.

import { type App, type Plan, type Variant, emit, makeCalls, planUsage, prepare, readPlan } from "./calls.ts";

export const cases: Record<string, { app: string; requests: string }> = {
  hono: { app: "./apps/hono/index.ts", requests: "./requests/hono.json" },
  elysia: { app: "./apps/elysia/index.ts", requests: "./requests/elysia.json" },
  "react-ssr": { app: "./apps/react-ssr/index.tsx", requests: "./requests/react-ssr.json" },
  "preact-ssr": { app: "./apps/preact-ssr/index.tsx", requests: "./requests/preact-ssr.json" },
  pdf: { app: "./apps/pdf/index.ts", requests: "./requests/pdf.ts" },
  webhook: { app: "./apps/webhook/index.ts", requests: "./requests/webhook.json" },
  express: { app: "./apps/express/index.ts", requests: "./requests/express.json" },
  postgres: { app: "./apps/postgres/index.ts", requests: "./requests/postgres.json" },
};

const usage = `Usage: NODE_ENV=production bun run.ts <${Object.keys(cases).join("|")}> ${planUsage}`;

function commandLine(argv: string[]): (Plan & { name: string }) | string {
  const [name, ...options] = argv;
  if (!name || !Object.hasOwn(cases, name)) return name ? `no case named ${name}` : "no case given";
  const plan = readPlan(options);
  return typeof plan === "string" ? plan : { name, ...plan };
}

// bench.ts imports the table above to write the built variants' entries, so the process runs only when Bun runs this
// file.
if (import.meta.main) {
  const parsed = commandLine(process.argv.slice(2));
  if (typeof parsed === "string") {
    emit(2, `run.ts: ${parsed}\n${usage}\n`);
    process.exit(2);
  }

  try {
    const paths = cases[parsed.name];
    const app: App = (await import(paths.app)).default;
    const variants = ((await import(paths.requests)).default as Variant[]).map(prepare);
    if (!variants.length) throw new Error(`${paths.requests} holds no request`);
    await makeCalls(app, variants, parsed);
  } catch (error) {
    emit(2, `run.ts: ${parsed.name}: ${error instanceof Error ? (error.stack ?? error.message) : String(error)}\n`);
    process.exit(1);
  }
}

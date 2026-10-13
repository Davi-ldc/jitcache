// A heavy CLI on fixed input: one pass transpiles 20 of Bun's own TypeScript modules with the TypeScript compiler's
// transpileModule. The compiler is about 9 MB of JavaScript, so this case starts the most code per process.

import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import ts from "typescript";
import type { Workload } from "../run.ts";

export async function setup(): Promise<Workload> {
  const directory = join(import.meta.dir, "../inputs/ts");
  const modules = readdirSync(directory, { recursive: true, encoding: "utf8" })
    .filter(path => path.endsWith(".ts"))
    .sort()
    .map(path => ({ path, source: readFileSync(join(directory, path), "utf8") }));
  if (!modules.length) throw new Error(`${directory} holds no module`);
  return {
    pass() {
      return modules.map(module => {
        // A build tool reports syntax errors, so the transpile collects them, and the inputs have none.
        const result = ts.transpileModule(module.source, {
          fileName: module.path,
          reportDiagnostics: true,
          compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext },
        });
        if (result.diagnostics?.length) throw new Error(`${module.path}: ${ts.flattenDiagnosticMessageText(result.diagnostics[0].messageText, "\n")}`);
        return result.outputText;
      });
    },
  };
}

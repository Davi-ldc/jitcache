// Reads what a run wrote to standard error for the counter checks of floor.js and polymorphic.js (SPEC-cb.md section 11.3).
// The script writes "<tag> invocation <k>" with printErr right before its k-th call of the body, and under verboseOSR
// operationOptimize writes, on entry,
//   <CodeBlock>: Entered optimize with bytecodeIndex = <index>, executeCounter = <count>/<threshold>, <m_counter>, ...
// where <CodeBlock> starts with the body's name and hash (CodeBlock::dumpAssumingJITType) and the counter part is
// ExecutionCounter::dump. Both go to the same unbuffered stderr from the VM thread, so their order is the order of events.
import { readFileSync } from "node:fs";

export interface OptimizeEntry {
  invocation: number | null; // the last invocation marker before the line, null when none came before it
  count: number; // ExecutionCounter::count()
  threshold: number; // m_activeThreshold
  counter: number; // m_counter
}

export interface OptimizeLog {
  invocations: Set<number>;
  entries: OptimizeEntry[];
}

// executionCounterIncrementForEntry, a fixed option (options.md): what each entry into baseline code adds to the counter.
export const entryIncrement = 15;

const escape = (text: string) => text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

export function readOptimizeLog(stderrPath: string, tag: string, name: string): OptimizeLog {
  const marker = new RegExp(`^${escape(tag)} invocation (\\d+)$`);
  const number = "(-?\\d+(?:\\.\\d+)?(?:e[-+]?\\d+)?)";
  const entry = new RegExp(`^${escape(name)}#[^:\\s]*:\\[[^\\]]*\\]: Entered optimize with bytecodeIndex = [^,]*, executeCounter = ${number}/${number}, (-?\\d+),`);
  const log: OptimizeLog = { invocations: new Set(), entries: [] };
  let invocation: number | null = null;
  for (const line of readFileSync(stderrPath, "utf8").split("\n")) {
    const markerMatch = marker.exec(line);
    if (markerMatch) {
      invocation = Number(markerMatch[1]);
      log.invocations.add(invocation);
      continue;
    }
    const entryMatch = entry.exec(line);
    if (entryMatch) log.entries.push({ invocation, count: Number(entryMatch[1]), threshold: Number(entryMatch[2]), counter: Number(entryMatch[3]) });
  }
  return log;
}

export function finish(problems: string[]) {
  if (!problems.length) return;
  console.error(problems.join("\n"));
  process.exit(1);
}

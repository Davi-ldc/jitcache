// The final statuses a run with JITCache writes to standard error, which the test runs and the bench mode both check.

import { type RunSpec, type Script, addresses, imports } from "./scripts.ts";

export type Fail = (run: number | null, check: string, detail: string) => void;

// Each fault the statuses report must be one a directive declares, and a run that imports must install some body unless
// jitcache-expect-no-install names it. Returns the fault steps the statuses report.
export function checkStatuses(script: Script, k: number, i: number, run: RunSpec, stderr: string, fail: Fail): Set<string> {
  const statuses: any[] = [];
  for (const line of stderr.split("\n")) {
    if (!line.startsWith('{"jitcache":"status"')) continue;
    try {
      statuses.push(JSON.parse(line));
    } catch {
      fail(i, "status", `a status line is no JSON: ${line}`);
    }
  }
  const reported = new Set<string>();
  if (!statuses.length) {
    fail(i, "status", "the run wrote no final status");
    return reported;
  }
  const allowed = new Set(script.faults.filter(f => addresses(f.at, k, i)).map(f => f.step));
  for (const status of statuses) {
    for (const fault of [status.activityFault, status.productionFault]) {
      if (!fault || reported.has(fault.step)) continue;
      reported.add(fault.step);
      if (!allowed.has(fault.step)) fail(i, "fault", `the status reports ${fault.class} at ${fault.step}, which no directive declares${fault.detail ? `: ${fault.detail}` : ""}`);
    }
  }
  if (imports(run.role) && !script.expectNoInstall.some(at => addresses(at, k, i))) {
    const installs = statuses.reduce((sum, status) => sum + Number(status.progress?.installs ?? 0), 0);
    if (!installs) fail(i, "install", `the ${run.role} installed no body`);
  }
  return reported;
}

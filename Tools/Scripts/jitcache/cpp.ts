// testjitcache's tests, one process per part and options string. A test's part is the directory under jitcache/tests/
// that holds its file, and the tests of one part with the same options share a process, in the order the listing gives
// them; the processes run in parallel under --jobs. A process that ends before each of its tests printed a verdict
// crashed in the first of them: those tests then run one process each, so the report names the test that was running
// and gives the later ones' verdicts, until two in a row crash or time out alone.

import { mkdtempSync, statSync } from "node:fs";
import { join } from "node:path";
import { type ProcessEnd, type ProcessIO, describeEnd, readText, tail } from "./launcher.ts";
import type { ReportedFailure, Session } from "./runs.ts";

// One "name<TAB>options<TAB>part<TAB>file" line of testjitcache --list, whose file is the registering file's path from
// Source/JavaScriptCore.
interface ListedTest {
  name: string;
  options: string;
  part: string;
  file: string; // from the repository's root
}

interface Group {
  part: string;
  options: string;
  tests: ListedTest[];
  selected: ListedTest[];
}

interface Verdict {
  passed: boolean;
  messages: string[]; // the file, line and message lines a failing test printed after its verdict
}

// A testjitcache process with these arguments, its output in files of its own.
type Testjitcache = (args: string[]) => Promise<{ end: ProcessEnd; stdout: string; stderr: string }>;

const endedCleanly = (end: ProcessEnd) => !end.error && !end.timedOut && end.code === 0;

function parseListing(stdout: string): ListedTest[] | string {
  const tests: ListedTest[] = [];
  for (const line of stdout.split("\n")) {
    if (!line) continue;
    const fields = line.split("\t");
    if (fields.length !== 4) return `testjitcache --list printed a line that is no name<TAB>options<TAB>part<TAB>file: ${JSON.stringify(line)}`;
    const [name, options, part, file] = fields;
    tests.push({ name, options, part, file: `Source/JavaScriptCore/${file}` });
  }
  return tests;
}

function verdicts(stdout: string): Map<string, Verdict> {
  const result = new Map<string, Verdict>();
  let current: Verdict | null = null;
  for (const line of stdout.split("\n")) {
    const verdict = /^(PASS|FAIL) (\S+)$/.exec(line);
    if (verdict) {
      current = { passed: verdict[1] === "PASS", messages: [] };
      result.set(verdict[2], current);
    } else if (current && line.startsWith("    ")) current.messages.push(line.trim());
  }
  return result;
}

export async function runCppTests(session: Session) {
  const { build, options } = session;
  if (!statSync(build.testjitcache, { throwIfNoEntry: false })?.isFile()) {
    session.listed.push("the C++ tests: the build has no bin/testjitcache, which only twins builds have");
    return;
  }
  const directory = mkdtempSync(join(session.base, "cpp-"));
  // A testjitcache process belongs to no sequence, and no twin check compares its heap with another's, so under QEMU each
  // draws its guest stack alone.
  let index = 0;
  const testjitcache: Testjitcache = async args => {
    const name = args[0] === "--list" ? "list" : `group${index++}`;
    const io: ProcessIO = { env: { ...process.env }, cwd: directory, stdout: join(directory, `${name}.stdout`), stderr: join(directory, `${name}.stderr`) };
    const end = await session.launcher.engine(build.testjitcache, args, io, null);
    return { end, stdout: readText(io.stdout) ?? "", stderr: readText(io.stderr) ?? "" };
  };
  const listing = await testjitcache(["--list"]);
  const tests = endedCleanly(listing.end) ? parseListing(listing.stdout) : `testjitcache --list ${describeEnd(listing.end)}: ${tail(listing.stderr)}`;
  if (typeof tests === "string") {
    session.failed++;
    session.keep();
    session.failures.push({ file: null, detail: tests });
    console.log(`FAIL ${tests}`);
    session.results.write({ kind: "cpp-list", outcome: "fail", detail: tests });
    return;
  }
  const groups = new Map<string, Group>();
  for (const test of tests) {
    const key = `${test.part}\t${test.options}`;
    let group = groups.get(key);
    if (!group) groups.set(key, (group = { part: test.part, options: test.options, tests: [], selected: [] }));
    group.tests.push(test);
    if (!options.filter || options.filter.test(`${test.part}/${test.name}`)) group.selected.push(test);
  }
  await Promise.all([...groups.values()].filter(group => group.selected.length).map(group => session.pool.run(() => runGroup(session, group, testjitcache))));
}

async function runGroup(session: Session, group: Group, testjitcache: Testjitcache) {
  const { part, options, selected } = group;
  const base = [`--part=${part}`, `--group=${options}`];
  // A group the filter splits still runs in one process, which names each selected test.
  const args = selected.length === group.tests.length ? base : [...base, ...selected.map(test => `--test=${test.name}`)];
  const run = await testjitcache(args);
  const results = verdicts(run.stdout);
  const failures: ReportedFailure[] = [];
  const problems: string[] = [];
  const failedTests: string[] = [];
  const fail = (test: ListedTest, verdict: Verdict) => {
    failedTests.push(test.name);
    failures.push({ file: test.file, detail: `${part}/${test.name} failed: ${verdict.messages.join("; ")}` });
  };
  for (const test of selected) {
    const verdict = results.get(test.name);
    if (verdict && !verdict.passed) fail(test, verdict);
  }
  if (!endedCleanly(run.end)) {
    const lines = run.stdout.split("\n").filter(line => line.startsWith("FAIL ") || line.startsWith("    "));
    problems.push(`${args.join(" ")} ${describeEnd(run.end)}${lines.length ? `\n${lines.join("\n")}` : `: ${tail(run.stderr)}`}`);
  }

  // The tests without a verdict: the one that was running when the process ended, and those after it. Each runs alone
  // until two in a row crash or time out alone, and the rest then do not run: a hang that every test of the group
  // shares, such as one in VM creation under the group's options, costs three timeouts instead of one per test.
  const unfinished = selected.filter(test => !results.has(test.name));
  const reruns: { test: string; outcome: "pass" | "fail" | "crash" | "not run"; end: string | null }[] = [];
  let crashesInARow = 0;
  for (const test of unfinished) {
    if (crashesInARow === 2) {
      reruns.push({ test: test.name, outcome: "not run", end: null });
      continue;
    }
    const alone = await testjitcache([...base, `--test=${test.name}`]);
    const verdict = verdicts(alone.stdout).get(test.name);
    const outcome = verdict && !verdict.passed ? "fail" : verdict && endedCleanly(alone.end) ? "pass" : "crash";
    crashesInARow = outcome === "crash" ? crashesInARow + 1 : 0;
    reruns.push({ test: test.name, outcome, end: describeEnd(alone.end) });
    const aloneProblem = outcome === "pass" ? null : `${[...base, `--test=${test.name}`].join(" ")} ${describeEnd(alone.end)}: ${verdict?.messages.join("; ") || tail(alone.stderr)}`;
    if (aloneProblem) problems.push(aloneProblem);
    if (test === unfinished[0]) {
      const byItself = outcome === "pass" ? "passes alone" : outcome === "fail" ? `fails alone: ${verdict!.messages.join("; ")}` : `${describeEnd(alone.end)} alone too: ${tail(alone.stderr, 4)}`;
      failures.push({ file: test.file, detail: `${part}/${test.name} was running when its group "${options}" ${describeEnd(run.end)}; it ${byItself}` });
      if (outcome === "fail") failedTests.push(test.name);
    } else if (outcome === "fail") fail(test, verdict!);
    else if (outcome === "crash") failures.push({ file: test.file, detail: `${part}/${test.name} ${describeEnd(alone.end)} alone: ${tail(alone.stderr, 4)}` });
  }
  const notRun = reruns.filter(rerun => rerun.outcome === "not run").map(rerun => rerun.test);
  if (notRun.length) failures.push({ file: `Source/JavaScriptCore/jitcache/tests/${part}/`, detail: `group ${part} "${options}" did not run ${notRun.join(", ")}, after two tests in a row crashed or timed out alone` });
  // A process that ended badly after every test's verdict, such as one LeakSanitizer reports at exit, fails its part.
  if (!endedCleanly(run.end) && !unfinished.length && !failures.length)
    failures.push({ file: `Source/JavaScriptCore/jitcache/tests/${part}/`, detail: `group ${part} "${options}" ${describeEnd(run.end)} after every test passed: ${tail(run.stderr, 4)}` });

  const failed = failures.length > 0 || problems.length > 0;
  const names = selected.map(test => test.name);
  session.results.write({
    kind: "cpp-group",
    part,
    options,
    tests: names,
    files: [...new Set(selected.map(test => test.file))],
    outcome: failed ? "fail" : "pass",
    failedTests,
    crashed: unfinished[0]?.name ?? null,
    reruns,
    problems,
  });
  if (failed) {
    session.failed++;
    session.keep();
    session.failures.push(...failures);
    console.log(`FAIL testjitcache ${part} "${options}"`);
    for (const problem of problems) console.log(problem.replace(/^/gm, "  "));
    if (unfinished.length) console.log(`  ran alone after the group ended: ${reruns.map(rerun => `${rerun.test} ${rerun.outcome}`).join(", ")}`);
  } else {
    session.passed++;
    console.log(`PASS testjitcache ${part} "${options}" (${names.length} tests)`);
  }
}

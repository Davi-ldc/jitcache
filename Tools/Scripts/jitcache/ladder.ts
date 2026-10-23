// The ladder: a twins build's whole verification in one command, under the machine lock the runner holds. Its rungs run
// in order, and a rung that fails ends the ladder once it finishes: testjitcache's tests; the corpus in twins mode,
// with the Bun-hosted scripts when --bun names Bun and the pin comparison when --pin names the pin's build; Bun's own
// tests when --bun names Bun; and the corpus in plain mode on the build --plain-build names. The ladder lists each
// failure of the rung that failed with the file it names, its results file holds a record per rung, and its last line
// gives the verdict with each rung that did not run and the reason, the pin comparison when no --pin named the pin's
// build, and the milestone sequences when no --milestone ran them.

import { mkdirSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, sep } from "node:path";
import { bunTestArguments, defaultBunTests, defaultMaxConcurrency } from "./bun-test.ts";
import { runCppTests } from "./cpp.ts";
import { bunSource } from "./env.ts";
import { type Build, describeEnd, readText, runProcess, tail } from "./launcher.ts";
import { ResultsLog } from "./results.ts";
import { CalibrationError, type ReportedFailure, Session, runScripts } from "./runs.ts";

type RungName = "cpp" | "corpora" | "bun" | "plain";

interface RungOutcome {
  failures: ReportedFailure[]; // none when the rung passed
  results: string; // where its details are: a session's results file, or the directory of Bun's run
}

// The failing tests of Bun's JUnit report, each with its file from the Bun checkout: a testcase element whose content
// opens with a failure element.
function failingBunTests(junit: string): { name: string; file: string }[] {
  const entities: Record<string, string> = { lt: "<", gt: ">", quot: '"', apos: "'", amp: "&" };
  const unescape = (text: string) => text.replace(/&(lt|gt|quot|apos|amp);/g, (_, entity: string) => entities[entity]);
  return [...junit.matchAll(/<testcase name="([^"]*)"[^>]*? file="([^"]*)"[^/>]*>\s*<failure/g)].map(match => ({ name: unescape(match[1]), file: unescape(match[2]) }));
}

// What a rung run through a session adds to the failures it reports.
async function sessionRung(session: Session, work: () => Promise<void>): Promise<RungOutcome> {
  const reported = session.failures.length;
  await work();
  return { failures: session.failures.slice(reported), results: session.results.path };
}

// The twins session, which runs the cpp and corpora rungs, comes from main, which built it from the command line.
export async function runLadder(twins: Session, plainBuild: Build | null): Promise<number> {
  const { options, build } = twins;
  const base = mkdtempSync(join(tmpdir(), "jitcache-ladder-"));
  const log = new ResultsLog(build.resultsDirectory, base, "ladder-");
  let calibrationError: string | null = null;
  let keepBase = false;
  let awaitingMilestone = 0; // the sequences the corpora and plain rungs left for --milestone

  const bunRung = async (): Promise<RungOutcome> => {
    const directory = join(base, "bun");
    mkdirSync(directory);
    const junit = join(directory, "junit.xml");
    const io = { env: { ...process.env }, cwd: bunSource, stdout: join(directory, "bun-test.stdout"), stderr: join(directory, "bun-test.stderr") };
    // --jobs bounds the tests that run at once, as it bounds the other rungs' processes, and so does bun-test.ts's own
    // limit, since each test starts Bun processes of its own.
    const concurrency = Math.min(options.jobs, defaultMaxConcurrency);
    const end = await runProcess(options.bun!, bunTestArguments(concurrency, defaultBunTests, junit), io, options.timeoutSeconds);
    const failures: ReportedFailure[] = failingBunTests(readText(junit) ?? "").map(test => ({ file: join(bunSource, test.file), detail: `${test.name} failed` }));
    const ended = describeEnd(end);
    if ((end.error || end.timedOut || end.code !== 0) && !failures.length) failures.push({ file: null, detail: `bun test ${ended}: ${tail(readText(io.stderr) || readText(io.stdout))}` });
    if (failures.length) keepBase = true;
    console.log(`bun test ${ended}; its output is in ${directory}`);
    return { failures, results: directory };
  };

  const rungs: { name: RungName; skip: string | null; run: () => Promise<RungOutcome> }[] = [
    { name: "cpp", skip: null, run: () => sessionRung(twins, () => runCppTests(twins)) },
    {
      name: "corpora",
      skip: null,
      run: () => sessionRung(twins, async () => {
        try {
          await runScripts(twins);
        } catch (error) {
          if (!(error instanceof CalibrationError)) throw error;
          twins.keep();
          calibrationError = error.message;
          twins.failures.push({ file: null, detail: `calibration: ${error.message}` });
        }
      }),
    },
    { name: "bun", skip: options.bun ? null : "no --bun", run: bunRung },
    {
      name: "plain",
      skip: plainBuild ? null : "no --plain-build",
      run: async () => {
        const plain = new Session({ ...options, build: plainBuild!.directory, mode: "plain", bun: null, pin: null, pinOptions: [] }, plainBuild!);
        const outcome = await sessionRung(plain, () => runScripts(plain));
        plain.finish(null);
        awaitingMilestone += plain.awaitingMilestone;
        return outcome;
      },
    },
  ];

  console.log(`ladder of ${build.directory}`);
  let failedRung: RungName | null = null;
  let failures: ReportedFailure[] = [];
  // What did not run, each with the reason, for the ladder's last line: the rungs, the pin comparison, which runs
  // inside the corpora rung, and the sequences a milestone cadence marks, which the corpora and plain rungs run only
  // under --milestone.
  const notRun: { what: string; reason: string }[] = [];
  for (const { name, skip, run } of rungs) {
    const reason = failedRung ? `the ${failedRung} rung failed` : skip;
    if (reason) {
      console.log(`ladder: ${name} not run: ${reason}`);
      log.write({ kind: "rung", rung: name, outcome: "not run", reason });
      notRun.push({ what: name, reason });
    } else {
      if (name === "corpora" && !twins.pinSetup) notRun.push({ what: "the pin comparison", reason: "no --pin" });
      console.log(`ladder: ${name}`);
      const started = performance.now();
      const outcome = await run();
      const seconds = Math.round((performance.now() - started) / 1000);
      const failed = outcome.failures.length > 0;
      console.log(`ladder: ${name} ${failed ? "failed" : "passed"} in ${seconds} s`);
      log.write({ kind: "rung", rung: name, outcome: failed ? "fail" : "pass", reason: null, seconds, failures: outcome.failures, results: outcome.results });
      if (failed) {
        failedRung = name;
        failures = outcome.failures;
      }
    }
    // The twins session serves the first two rungs and prints its summary after them.
    if (name === "corpora") {
      twins.finish(calibrationError);
      awaitingMilestone += twins.awaitingMilestone;
    }
  }
  if (awaitingMilestone) notRun.push({ what: "the milestone sequences", reason: "no --milestone" });

  for (const failure of failures) console.log(`FAIL ${failure.file ? `${failure.file}: ` : ""}${failure.detail}`);
  const exit = failedRung ? 1 : 0;
  log.write({ kind: "ladder", build: build.directory, outcome: failedRung ? "fail" : "pass", failedRung, notRun, exit });
  if (keepBase || log.path.startsWith(`${base}${sep}`)) console.log(`kept: ${base}`);
  else rmSync(base, { recursive: true, force: true });
  console.log(`ladder results: ${log.path}`);
  const notRunText = notRun.length ? `; not run: ${notRun.map(({ what, reason }) => `${what} (${reason})`).join(", ")}` : "";
  console.log(`ladder: ${failedRung ? `failed at ${failedRung}` : "passed"}${notRunText}`);
  return exit;
}

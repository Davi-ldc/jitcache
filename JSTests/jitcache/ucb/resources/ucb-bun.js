// Shared by the UCB lane's Bun-hosted tests (SPEC-ucb.md section 13.3): require("./resources/ucb-bun.js").
//
// A Bun-hosted run gets its role, scratch path, artifact path and sequence index as process.argv[2] to [5]
// (SPEC-integrator.harness.md section 7.7). The context reads JITCache's state only in a run that configures JITCache
// and only through the bun:jsc exports a twins build adds (harness section 5.3); elsewhere every reader returns null.
//
// Some tests run a `bun build --compile` executable, the "app", in a child process. The app is a JITCache VM of its
// own: it gets the run's role, strictness and twin-report path through BUN_OPTIONS, which standalone executables read,
// with an artifact of its own beside the run's, since the test's own VM holds the producer lock of the run's artifact.
// A test can also start it as a Consumer of the run's artifact, which needs no lock (runApp, overRunArtifact). Its twin
// report is the run's, appended to, its final status lines go to the run's standard error, and it avoids the layouts of
// the processes whose captures it can import, so the runner's checks and placement cover it as they cover the test's
// own VM (harness sections 4, 7.5 and 7.7). Its standard output is printed by the test, so the oracle compares it.
"use strict";

const fs = require("node:fs");
const path = require("node:path");

let jsc = { };
try {
    jsc = require("bun:jsc");
} catch { }

function valueAt(object, keyPath)
{
    let value = object;
    for (const name of keyPath.split(".")) {
        if (value === null || value === undefined)
            return undefined;
        value = value[name];
    }
    return value;
}

function difference(before, after)
{
    const result = { };
    for (const name of Object.keys(after)) {
        const value = after[name];
        const earlier = before ? before[name] : undefined;
        if (typeof value === "number")
            result[name] = value - (typeof earlier === "number" ? earlier : 0);
        else if (value && typeof value === "object")
            result[name] = difference(earlier, value);
    }
    return result;
}

function matches(actual, expected)
{
    if (typeof expected === "number")
        return actual === expected;
    const [operator, bound] = expected;
    switch (operator) {
    case ">=":
        return actual >= bound;
    case "<=":
        return actual <= bound;
    case ">":
        return actual > bound;
    case "<":
        return actual < bound;
    }
    throw new Error(`unknown comparison ${operator}`);
}

// Whether this Bun is instrumented with AddressSanitizer, as Bun's own test harness tells (test/harness.ts).
function isASAN()
{
    try {
        const { isASANEnabled } = require("bun:internal-for-testing");
        if (typeof isASANEnabled === "function")
            return isASANEnabled();
    } catch { }
    return /asan|debug/.test(path.basename(process.execPath)) || Bun.version.includes("debug");
}

class Context {
    constructor(role, scratch, artifact, sequence)
    {
        this.role = role;
        this.scratch = scratch;
        this.artifact = artifact;
        this.sequence = Number(sequence);
        this.off = role === "Off";
        this.configured = role === "Producer" || role === "ConsumerProducer" || role === "Consumer";
        this.produces = role === "Producer" || role === "ConsumerProducer";
        this.imports = role === "Consumer" || role === "ConsumerProducer";
        this.observes = this.configured && typeof jsc.jitcacheUCBStatistics === "function";
        this.twins = this.configured && typeof jsc.jitcacheStatus === "function";
    }

    check(condition, message)
    {
        if (!condition)
            throw new Error(`${this.role}: ${typeof message === "function" ? message() : message}`);
    }

    statistics()
    {
        return this.observes ? jsc.jitcacheUCBStatistics() : null;
    }

    verifyRegistry()
    {
        return this.observes ? jsc.jitcacheUCBStatistics({ verifyRegistry: true }).registryViolations : null;
    }

    // Every test, and every app a test runs, ends here: after a full collection, the registry holds no entry for a dead
    // cell and every child UFE of a recorded UCB has its identity (verifyRegistry, SPEC-ucb.md section 13.2), which
    // section 13.3 asks of each test's stress runs too.
    finish()
    {
        const violations = this.verifyRegistry();
        this.check(violations === null || violations === 0, `verifyRegistry found ${violations} violations`);
    }

    delta(before, after)
    {
        return before && after ? difference(before, after) : null;
    }

    expect(label, delta, expectations)
    {
        if (!delta)
            return;
        const failures = [];
        for (const [keyPath, expected] of Object.entries(expectations)) {
            const actual = valueAt(delta, keyPath) ?? 0;
            if (!matches(actual, expected))
                failures.push(`${keyPath} changed by ${actual}, expected ${typeof expected === "number" ? expected : expected.join(" ")}`);
        }
        if (failures.length)
            throw new Error(`${this.role}: ${label}: ${failures.join("; ")}; delta ${JSON.stringify(delta)}`);
    }

    window(label, action, expectations)
    {
        const before = this.statistics();
        const value = action();
        const after = this.statistics();
        this.expect(label, this.delta(before, after), expectations);
        return value;
    }

    async windowAsync(label, action, expectations)
    {
        const before = this.statistics();
        const value = await action();
        const after = this.statistics();
        this.expect(label, this.delta(before, after), expectations);
        return value;
    }

    status()
    {
        return this.twins ? jsc.jitcacheStatus() : undefined;
    }

    progress()
    {
        return this.twins ? jsc.jitcacheProgress() : null;
    }

    file(name)
    {
        return path.join(this.scratch, name);
    }

    // `bun build --compile` of `entries` into the scratch directory, once per sequence: every run of the sequence, the
    // oracle's included, runs the same executable. `format` passes --format: without it a build is ES modules, and with
    // --bytecode CommonJS (src/runtime/cli/Arguments.rs). `transform`, when given, rewrites the built file's bytes in place.
    buildApp(name, entries, { bytecode = false, format = null, transform = null } = { })
    {
        const outfile = this.file(name);
        if (fs.existsSync(outfile))
            return outfile;
        const command = [process.execPath, "build", "--compile", ...(bytecode ? ["--bytecode"] : []), ...(format ? [`--format=${format}`] : []), ...entries, "--outfile", outfile];
        const environment = { ...process.env };
        delete environment.BUN_OPTIONS;
        const built = Bun.spawnSync({ cmd: command, env: environment, stdout: "pipe", stderr: "pipe" });
        if (built.exitCode !== 0)
            throw new Error(`${command.join(" ")} exited with ${built.exitCode}: ${built.stderr.toString()}${built.stdout.toString()}`);
        if (transform) {
            const bytes = fs.readFileSync(outfile);
            transform(bytes);
            fs.writeFileSync(outfile, bytes);
        }
        return outfile;
    }

    // The JITCache flags an app gets from this run, as the runner passed them (harness section 7.7). An app over an
    // artifact of its own runs this run's role, records its layout beside the run's and avoids the layouts the apps of
    // earlier runs recorded. An app over the run's artifact runs as a Consumer, records no layout, since it captures
    // nothing, and avoids the layouts of this run's process and of every earlier run, the processes whose captures it can
    // import (harness sections 4 and 7.3).
    appFlags(appArtifact, overRunArtifact)
    {
        if (!this.configured)
            return [];
        const flags = [`--jitcache=${appArtifact}`];
        let sawArtifact = false;
        let recordedLayout = null;
        let avoidedLayouts = null;
        for (const argument of process.execArgv) {
            const [name, value] = argument.split(/=(.*)/s);
            switch (name) {
            case "--jitcache":
                sawArtifact = true;
                break;
            case "--jitcache-mode":
                flags.push(overRunArtifact ? `${name}=c` : argument);
                break;
            case "--jitcache-strict":
            case "--jitcache-twins-report":
                flags.push(argument);
                break;
            case "--jitcache-twins-record-layout":
                recordedLayout = value;
                break;
            case "--jitcache-twins-avoid-layout":
                avoidedLayouts = (value || "").split(",").filter(Boolean);
                break;
            }
        }
        this.check(sawArtifact, `the run's execArgv names no --jitcache: ${process.execArgv.join(" ")}`);
        const avoid = layouts => `--jitcache-twins-avoid-layout=${layouts.filter(file => fs.existsSync(file)).join(",")}`;
        if (overRunArtifact) {
            if (avoidedLayouts !== null || recordedLayout !== null)
                flags.push(avoid([...(avoidedLayouts ?? []), ...(recordedLayout !== null ? [recordedLayout] : [])]));
            return flags;
        }
        if (recordedLayout !== null)
            flags.push(`--jitcache-twins-record-layout=${recordedLayout}.app`);
        if (avoidedLayouts !== null)
            flags.push(avoid(avoidedLayouts.map(file => `${file}.app`)));
        return flags;
    }

    // Runs the app and prints its standard output. By default the app runs this run's role over an artifact of its own.
    // With `overRunArtifact`, a run that configures JITCache starts it as a Consumer of this run's artifact, which
    // consumers open without a lock, so the app imports or seeds the bodies the test's own VM captured; an Off run starts
    // it without JITCache either way. The app reads its role and scratch path from the environment, since a standalone
    // executable's argv is its user's.
    runApp(executable, { artifactName = "app-artifact", overRunArtifact = false, flags = [], environment = { } } = { })
    {
        let appArtifact = this.artifact;
        if (!overRunArtifact) {
            appArtifact = this.file(artifactName);
            fs.mkdirSync(appArtifact, { recursive: true });
        }
        const options = [...this.appFlags(appArtifact, overRunArtifact), ...flags];
        const role = overRunArtifact && this.configured ? "Consumer" : this.role;
        const env = { ...process.env, ...environment, JITCACHE_UCB_ROLE: role, JITCACHE_UCB_SCRATCH: this.scratch, JITCACHE_UCB_SEQUENCE: String(this.sequence) };
        if (options.length)
            env.BUN_OPTIONS = options.join(" ");
        else
            delete env.BUN_OPTIONS;
        const ran = Bun.spawnSync({ cmd: [executable], env, stdout: "pipe", stderr: "inherit" });
        process.stdout.write(ran.stdout.toString());
        if (ran.exitCode !== 0)
            throw new Error(`${path.basename(executable)} exited with ${ran.exitCode} in the ${this.role} run`);
    }
}

function context(argv = process.argv)
{
    return new Context(argv[2], argv[3], argv[4], argv[5]);
}

// For code inside an app: its role and scratch path from the environment runApp sets.
function appContext()
{
    return new Context(process.env.JITCACHE_UCB_ROLE, process.env.JITCACHE_UCB_SCRATCH, "", process.env.JITCACHE_UCB_SEQUENCE ?? "0");
}

module.exports = { context, appContext, isASAN, Context };

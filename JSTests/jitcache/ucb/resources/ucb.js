// Shared by the UCB lane's jsc-hosted tests (SPEC-ucb.md section 13.3), loaded with
// load("./resources/ucb.js", "caller relative"). It defines one global, ucbTest, whose value does not depend on the run's
// role, so the heap the runner describes after the run stays the same in every role (SPEC-integrator.md R-ALL-4). A test
// calls ucbTest(role, scratch, artifact, sequence) inside its main function and keeps the context only there.
//
// The context reads JITCache's state only in a run that configures JITCache and only through the helpers a twins build
// registers ($vm.jitCacheUCBStatistics and the jitcache* shell functions of SPEC-integrator.harness.md section 5); in a
// plain build, and under the Off role, every reader returns null and the assertions about JITCache's state are skipped.
// The one exception is events(), whose counts describe native behavior and so serve every role of a twins build.
var ucbTest = (function () {
    "use strict";

    const hasStatistics = typeof $vm === "object" && $vm !== null && typeof $vm.jitCacheUCBStatistics === "function";
    const hasShellFunctions = typeof jitcacheBodyKey === "function";

    function valueAt(object, path)
    {
        let value = object;
        for (const name of path.split(".")) {
            if (value === null || value === undefined)
                return undefined;
            value = value[name];
        }
        return value;
    }

    // The difference of two statistics objects of the same shape, counter by counter.
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

    function describe(value)
    {
        try {
            return JSON.stringify(value);
        } catch {
            return String(value);
        }
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
            // Reads of JITCache's state: only with JITCache configured and in a twins build (R-ALL-4).
            this.observes = this.configured && hasStatistics;
            this.twins = this.configured && hasShellFunctions;
        }

        check(condition, message)
        {
            if (!condition)
                throw new Error(`${this.role}: ${typeof message === "function" ? message() : message}`);
        }

        statistics()
        {
            return this.observes ? $vm.jitCacheUCBStatistics() : null;
        }

        verifyRegistry()
        {
            return this.observes ? $vm.jitCacheUCBStatistics({ verifyRegistry: true }).registryViolations : null;
        }

        // Every test ends here: after a full collection, the registry holds no entry for a dead cell and every child UFE
        // of a recorded UCB has the identity its parent's record gives it (verifyRegistry, SPEC-ucb.md section 13.2), which
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

        // Checks each expectation against a statistics delta. An expectation maps a counter's path ("imports",
        // "misses.NoBody", "counts.codeBlocks") to an exact number or to [comparison, bound].
        expect(label, delta, expectations)
        {
            if (!delta)
                return;
            const failures = [];
            for (const [path, expected] of Object.entries(expectations)) {
                const actual = valueAt(delta, path) ?? 0;
                if (!matches(actual, expected))
                    failures.push(`${path} changed by ${actual}, expected ${typeof expected === "number" ? expected : expected.join(" ")}`);
            }
            if (failures.length)
                throw new Error(`${this.role}: ${label}: ${failures.join("; ")}; delta ${describe(delta)}`);
        }

        // Runs `action` between two statistics reads and checks the delta. The action's own first call is a request
        // for its body, which misses NoBody and records a generated UCB, so a window that counts those uses explicit
        // reads instead.
        window(label, action, expectations)
        {
            const before = this.statistics();
            const value = action();
            const after = this.statistics();
            this.expect(label, this.delta(before, after), expectations);
            return value;
        }

        status()
        {
            return this.twins ? jitcacheStatus() : undefined;
        }

        progress()
        {
            return this.twins ? jitcacheProgress() : null;
        }

        bodyKey(fn, kind = "call")
        {
            return this.twins ? jitcacheBodyKey(fn, kind) : null;
        }

        // The event counts of the UCB of fn's CodeBlock of `kind` (SPEC-integrator.harness.md section 10.2). They count
        // what the engine did, so a test may assert them in every role, Off included (SPEC-integrator.md R-ALL-4),
        // wherever a twins build registers the function; null elsewhere, the pin's build included.
        events(fn, kind = "call")
        {
            return hasShellFunctions ? jitcacheBodyEvents(fn, kind) : null;
        }

        readSection(key, name)
        {
            if (!this.twins || !key)
                return null;
            const buffer = jitcacheReadSection(key, name);
            return buffer ? new Uint8Array(buffer) : null;
        }

        rewriteSection(key, name, offset, bytes)
        {
            this.check(this.twins && this.produces, `rewriting ${name} needs a producing twins run`);
            jitcacheRewriteSection(key, name, offset, Array.from(bytes));
        }

        // jitcacheDelta() in a producing twins run; null elsewhere, since R-ALL-4 keeps the Off role from calling it.
        captureDelta()
        {
            return this.twins && this.produces ? jitcacheDelta() : null;
        }

        // Files in the sequence's scratch directory, which every run of the sequence and its oracle runs share.
        path(name)
        {
            return `${this.scratch}/${name}`;
        }

        hasFile(name)
        {
            try {
                readFile(this.path(name), "binary");
                return true;
            } catch {
                return false;
            }
        }

        readText(name)
        {
            try {
                return readFile(this.path(name));
            } catch {
                return null;
            }
        }

        writeText(name, text)
        {
            writeFile(this.path(name), text);
        }

        describe(value)
        {
            return describe(value);
        }
    }

    return function ucbTest(role, scratch, artifact, sequence)
    {
        return new Context(role, scratch, artifact, sequence);
    };
})();

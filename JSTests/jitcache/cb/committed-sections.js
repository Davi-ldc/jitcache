// jitcache-runs: Producer --jitcache-delta-at-exit; ConsumerProducer --jitcache-delta-at-exit; Consumer

// SPEC-cb.md section 11.3, I13: every cb.state a producer commits passes V1 to V15 and S3 against the CB it came from.
// SC1 checks that at each capture, since every corpus run has strict on, and a failure would end production with a
// fault the runner does not allow; this script makes captures at every point that commits: the baseline compilations'
// captures, deltas after more types, after a full collection's drain and after a jettison, and in a ConsumerProducer the
// recaptures of imported CBs. In twins builds, after each delta and in the Consumer, it also reads back every watched
// body's committed sections and checks the rules no CodeBlock is needed for, and the summary against its cb.state (I8,
// I10, I16), with resources/cb-sections.js.
(function main(role) {
    function arithmetic(a, b) {
        return a * b + a / (b + 1);
    }
    function properties(object) {
        return object.alpha + (object.beta ?? 0);
    }
    function arrays(values, n) {
        const copy = [n, ...values];
        copy[n & 3] = n * 0.5;
        return copy.length + copy[0] + [1, 2, 3][n % 3];
    }
    function iterate(iterable) {
        let total = 0;
        for (const item of iterable)
            total += typeof item === "number" ? item : item.length;
        return total;
    }
    function enumerate(object) {
        let total = 0;
        for (const key in object)
            total += object[key] | 0;
        return total;
    }
    function Pair(left, right) {
        this.left = left;
        this.right = right;
    }
    function identity(value) {
        return value;
    }
    function stormy(x) {
        return x * 5 - 2;
    }
    noInline(stormy);
    const watched = [[arithmetic, "call"], [properties, "call"], [arrays, "call"], [iterate, "call"], [enumerate, "call"], [Pair, "construct"], [identity, "call"], [stormy, "call"]];

    const checking = role !== "Off" && typeof jitcacheBodyEvents === "function";
    const sections = checking ? load("./resources/cb-sections.js", "caller relative") : null;
    const producing = checking && (role === "Producer" || role === "ConsumerProducer");
    // Runs delta when this run produces, then checks every watched body that has a committed one; after the first
    // phase every watched body must have one.
    function deltaAndCheck(phase, everyBody) {
        if (!checking)
            return;
        if (producing)
            jitcacheDelta();
        for (const [fn, kind] of watched) {
            const label = `${phase}: ${fn.name} (${kind})`;
            if (everyBody)
                sections.checkBody(fn, kind, label);
            else if (sections.readBody(fn, kind))
                sections.checkBody(fn, kind, label);
        }
    }

    function phase(count, variant) {
        let total = 0;
        for (let i = 0; i < count; ++i) {
            total += arithmetic(variant ? i + 0.5 : i, 3);
            total += properties(variant && i & 1 ? { alpha: i, beta: 2 } : { alpha: i });
            total += arrays([i, 2, 3], i);
            total += iterate(variant ? "abc" : [i, 1]);
            total += enumerate(variant ? [i, 1] : { a: i, b: 1 });
            const pair = new Pair(i, variant ? "r" : i);
            total += pair.left;
            total += `${identity(variant ? `v${i & 3}` : i)}`.length;
            total += stormy(i);
        }
        return total;
    }

    let total = phase(120, false);
    deltaAndCheck("baseline compilations", role === "Producer");
    total += phase(60, true);
    fullGC();
    deltaAndCheck("after more types and a drain", role === "Producer");

    // stormy compiles to the DFG on integers, then exits on doubles until its DFG code is jettisoned, so delta captures
    // its baseline CB with a reoptimization counted. How many calls that takes depends on what the run imported, so
    // nothing printed depends on them.
    for (let i = 0; i < 100000 && !numberOfDFGCompiles(stormy); ++i)
        stormy(i);
    for (let i = 0; i < 100000 && !reoptimizationRetryCount(stormy); ++i)
        stormy(i + 0.5);
    deltaAndCheck("after a jettison", role === "Producer");
    print(total);
})(...arguments);

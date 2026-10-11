// jitcache-runs: Producer --jitcache-delta-at-exit --collectContinuously=true --useConcurrentJIT=false; ConsumerProducer --jitcache-delta-at-exit --collectContinuously=true --useConcurrentJIT=false; Consumer --collectContinuously=true --useConcurrentJIT=false
// jitcache-runs: Producer --jitcache-delta-at-exit --collectContinuously=true --useConcurrentJIT=true; ConsumerProducer --jitcache-delta-at-exit --collectContinuously=true --useConcurrentJIT=true; Consumer --collectContinuously=true --useConcurrentJIT=true

// SPEC-cb.md section 11.3, concurrency, in two sequences whose every run collects continuously, so markers drain and
// merge profiles while captures read them and while imports seed them (N3, N11). Each sequence captures in delta in its
// Producer and its ConsumerProducer, which also imports, as does its Consumer; the capture and restoration paths assert
// I2 and I3 in debug-local (AssertNoGC), and no run may crash.
// - The first sequence keeps concurrent JIT off. Every lane's twins pass; this lane's run before installCode, before any
//   marker can reach the CB.
// - The second turns it on, so DFG compiles run beside the captures and the imports. Its output must equal the
//   JITCache-off run's, and this lane's twins still pass, since its counter check compares with what finishCounter
//   decided (section 11.1). The Image lane's check skips every import here, and the runner treats those skips as no
//   difference because the runs turn concurrent JIT on.
// The six bodies of the rounds are first called at the start of round 0, before the run has compiled anything to the
// DFG, so their imports meet a DFG compile only by chance. The chain bodies arrange the meeting. Each is first called at
// the end of round 2, once and right after the one before it, and loops long enough to cross into the DFG during that
// call. The crossing comes at a loop hint, so operationOptimize compiles the body even where it would otherwise wait for a
// caller to inline it, and the compile takes far longer than the rest of the loop. With concurrent JIT on, a worker is
// thus still compiling each chain body while the next one's first call imports and installs it, in the ConsumerProducer
// and the Consumer. In twins builds the producing roles also run delta after each round, while the collector and
// compiler threads work; in the ConsumerProducer, the delta after round 2 captures the imported chain bodies right after
// the last one's compile started. Twins builds also check, in the ConsumerProducer and the Consumer, that every chain
// body was imported at its first call.
(function main(role) {
    function sumFields(objects) {
        let total = 0;
        for (let i = 0; i < objects.length; ++i)
            total += objects[i].weight + (objects[i].bonus ?? 0);
        return total;
    }
    function mixArithmetic(a, b) {
        return a * b + (a / (b + 1)) - (a % 7);
    }
    function iterateAll(iterable) {
        let total = 0;
        for (const item of iterable)
            total += typeof item === "number" ? item : item[1];
        return total;
    }
    function countKeys(object) {
        let count = 0;
        for (const key in object)
            count += object[key] & 1;
        return count;
    }
    function buildArrays(n) {
        const made = [n, n + 0.5];
        const literal = [1, 2, 3];
        literal[n % 3] = n * 0.25;
        return made.length + literal[0] + new Array(n & 7).length;
    }
    function Particle(x, y) {
        this.x = x;
        this.y = y;
    }

    // The chain bodies, each a body of its own with its own key. In the Producer the loop of a chain body's one call takes
    // it from the LLInt to baseline, whose compilation's capture commits it; an imported chain body crosses its DFG
    // threshold, about a thousand points, within a fraction of chainIterations.
    const chain = [
        n => {
            let sum = 0;
            for (let i = 0; i < n; ++i)
                sum += i & 3;
            return sum;
        },
        n => {
            let sum = 1;
            for (let i = 0; i < n; ++i)
                sum += (i ^ 5) & 7;
            return sum;
        },
        n => {
            let sum = 2;
            for (let i = 0; i < n; ++i)
                sum = (sum + i * 3) & 0xffff;
            return sum;
        },
        n => {
            let sum = 3;
            for (let i = 0; i < n; ++i)
                sum += i % 5;
            return sum;
        },
    ];
    const chainIterations = 6000;

    const objects = [];
    for (let i = 0; i < 16; ++i)
        objects.push(i & 1 ? { weight: i, bonus: 1 } : { weight: i });
    const map = new Map([[1, 2], [3, 4]]);
    const set = new Set([5, 6, 7]);
    const keyed = { a: 1, b: 2, c: 3 };
    const producing = (role === "Producer" || role === "ConsumerProducer") && typeof jitcacheDelta === "function";

    let total = 0;
    for (let round = 0; round < 8; ++round) {
        for (let i = 0; i < 60; ++i) {
            total += sumFields(objects);
            total += mixArithmetic(i, round + 1);
            total += iterateAll(i & 1 ? map : set) + iterateAll([i, round]);
            total += countKeys(i & 3 ? keyed : [i, i + 1]);
            total += buildArrays(i);
            const particle = new Particle(i, round * 0.5);
            total += particle.x + particle.y;
        }
        if (round === 2) {
            for (const body of chain)
                total += body(chainIterations);
        }
        if (producing)
            jitcacheDelta();
    }
    print(total);

    if ((role === "ConsumerProducer" || role === "Consumer") && typeof jitcacheBodyEvents === "function") {
        const sections = load("./resources/cb-sections.js", "caller relative");
        chain.forEach((body, index) => sections.expectImported(body, "call", `chain body ${index}`));
    }
})(...arguments);

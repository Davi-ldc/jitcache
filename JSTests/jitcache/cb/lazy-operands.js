// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// SPEC-cb.md section 11.3, lazy-operand profiles (P3), which only DFG OSR exits create (N7), forced before delta runs.
// An exit on a structure check whose operand is a GetLocal records a lazy-operand profile in the baseline CB of the
// exiting node's inline frame, keyed by that GetLocal's bytecode index and the operand it reads
// (Graph::methodOfGettingAValueProfileFor):
// - In the root frame, lazyRoot checks the structure of its local `inner` inside a loop. Holders whose inner object has
//   another structure exit there on every call until the DFG code is jettisoned, which makes lazyRoot's baseline CB its
//   executable's replacement again, so delta captures it.
// - Inside an inlined callee, lazyCallee runs for-of over a user-defined iterator, inlined into lazyCaller, which iterates
//   a map and a set first. The DFG's map and set iteration takes private tmps (ByteCodeParser::allocatePrivateTmps), so
//   the inlinee's tmps start past the caller's own tmps and its private ones (tmpOffsetForInlineeOf). The inlinee exits
//   on the structure check of its argument `weights`, a machine-frame local past its own numCalleeLocals(), and on the
//   structure check of the checkpoint tmp that holds next()'s result, a tmp at maxNumCheckpointTmps or above. next() is
//   never inlined, so its result reaches that check through the tmp. lazyCallee never gets DFG code of its own: it runs
//   only inlined, so its baseline CB stays its executable's replacement.
// The Producer commits both CBs with strict on, and in twins builds checks their lazy-operand records; the Consumer
// installs them, passes their twins, and in twins builds checks that they were imported.
(function main(role) {
    class Countdown {
        constructor(count, tagged) {
            this.count = count;
            this.tagged = tagged;
        }
        [Symbol.iterator]() {
            return this;
        }
        next() {
            if (this.count <= 0)
                return this.tagged ? { done: true, value: undefined, tag: 1 } : { done: true, value: undefined };
            const value = this.count--;
            return this.tagged ? { done: false, value, tag: 1 } : { done: false, value };
        }
    }

    function lazyCallee(iterable, weights) {
        let total = 0;
        for (const value of iterable)
            total += value * weights.scale;
        return total;
    }
    function lazyCaller(map, set, iterable, weights) {
        let total = 0;
        for (const entry of map)
            total += entry[1];
        for (const item of set)
            total += item;
        return total + lazyCallee(iterable, weights);
    }
    function lazyRoot(holder, rounds) {
        const inner = holder.inner;
        let total = 0;
        for (let i = 0; i < rounds; ++i)
            total += inner.weight;
        return total;
    }
    noInline(Countdown.prototype.next);
    noInline(lazyCaller);
    noInline(lazyRoot);

    // The caller iterates more than the callee on every call, so it reaches each tier first, and the DFG compilation
    // that inlines the callee comes before the callee's own counter would cross.
    const map = new Map([[1, 2], [3, 4], [5, 6], [7, 8]]);
    const set = new Set([9, 10, 11, 12]);
    const weights = { scale: 2 };
    const steady = { inner: { weight: 1 } };

    if (role === "Producer") {
        for (let i = 0; i < 100000 && !numberOfDFGCompiles(lazyCaller); ++i)
            lazyCaller(map, set, new Countdown(2, false), weights);
        if (!numberOfDFGCompiles(lazyCaller) || numberOfDFGCompiles(lazyCallee))
            throw new Error(`lazyCaller has ${numberOfDFGCompiles(lazyCaller)} DFG compilations and lazyCallee ${numberOfDFGCompiles(lazyCallee)}; the callee must run only inlined`);
        // A `weights` of another structure, then results of another structure.
        for (let i = 0; i < 4; ++i)
            lazyCaller(map, set, new Countdown(2, false), { scale: 2, extra: i });
        for (let i = 0; i < 4; ++i)
            lazyCaller(map, set, new Countdown(2, true), weights);

        for (let i = 0; i < 100000 && !numberOfDFGCompiles(lazyRoot); ++i)
            lazyRoot(steady, 3);
        for (let i = 0; i < 100000 && !reoptimizationRetryCount(lazyRoot); ++i)
            lazyRoot({ inner: { weight: 1, other: i } }, 3);
        if (!reoptimizationRetryCount(lazyRoot))
            throw new Error("lazyRoot's exits never jettisoned its DFG code");

        // The collection's finalization drains the lazy-operand profiles' samples into their predictions.
        fullGC();
        if (typeof jitcacheDelta === "function") {
            jitcacheDelta();
            const sections = load("./resources/cb-sections.js", "caller relative");
            const { OperandKind } = sections;
            const root = sections.checkBody(lazyRoot, "call", "lazyRoot").state;
            if (!root.lazyOperands.some(record => record.kind === OperandKind.Local))
                throw new Error(`lazyRoot: no lazy-operand record names a local: ${sections.describe(root.lazyOperands)}`);
            if (!root.header.reoptimizationRetryCounter)
                throw new Error("lazyRoot: the committed capture predates the jettison");
            const callee = sections.checkBody(lazyCallee, "call", "lazyCallee").state;
            if (!callee.lazyOperands.some(record => record.kind === OperandKind.Local))
                throw new Error(`lazyCallee: no lazy-operand record names the inlined argument's machine-frame local: ${sections.describe(callee.lazyOperands)}`);
            if (!callee.lazyOperands.some(record => record.kind === OperandKind.Tmp && record.value >= sections.maxNumCheckpointTmps))
                throw new Error(`lazyCallee: no lazy-operand record names a tmp at maxNumCheckpointTmps or above: ${sections.describe(callee.lazyOperands)}`);
        }
    }

    // Every role: the bodies run hot with the shapes of both phases, so the Consumer's own DFG compilations parse the
    // imported lazy-operand predictions.
    let total = 0;
    for (let i = 0; i < 60; ++i) {
        const otherWeights = i % 10 === 8 ? { scale: 3, extra: i } : weights;
        total += lazyCaller(map, set, new Countdown(2, i % 10 === 9), otherWeights);
        total += lazyRoot(i % 10 === 7 ? { inner: { weight: 2, other: i } } : steady, 3);
    }
    print(total);

    if (role === "Consumer" && typeof jitcacheBodyEvents === "function") {
        const sections = load("./resources/cb-sections.js", "caller relative");
        sections.expectImported(lazyCaller, "call", "lazyCaller");
        sections.expectImported(lazyCallee, "call", "lazyCallee");
        sections.expectImported(lazyRoot, "call", "lazyRoot");
    }
})(...arguments);

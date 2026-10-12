// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --maximumFunctionForCallInlineCandidateBytecodeCostForDFG=170; Consumer --useConcurrentJIT=false --maximumFunctionForCallInlineCandidateBytecodeCostForDFG=170

// SPEC-cb.md section 11.3, lazy-operand profiles (P3), which only DFG OSR exits create (N7), forced before delta runs.
// An exit on a type check whose operand is a GetLocal records a lazy-operand profile in the baseline CB of the exiting
// node's inline frame, keyed by that GetLocal's bytecode index and the operand it reads
// (Graph::methodOfGettingAValueProfileFor). A structure check records none, because SpeculativeJIT::emitStructureCheck
// passes speculationCheck no node. A local that held only strings is flushed as a cell, so reading its length checks
// the GetLocal for a string, and an array, a cell but not a string, fails that check:
// - In the root frame, lazyRoot reads the length of its local `word` inside a loop. Holders whose word is an array exit
//   there on every call until the DFG code is jettisoned, which makes lazyRoot's baseline CB its executable's
//   replacement again, so delta captures it.
// - Inside an inlined callee: lazyCaller inlines lazyCallee, which reads the length of its argument `unit` in a for-of
//   loop. The inlinee reads `unit` from lazyCaller's outgoing argument area, a local of the machine frame
//   (InlineCallFrame::stackOffset), and the bindings lazyCaller keeps live across the call put that area past
//   lazyCallee's own numCalleeLocals(), so the exit's key names a local outside lazyCallee's frame. lazyCallee never
//   gets DFG code of its own (the comment on the order of tier-ups below says why), so its baseline CB stays its
//   executable's replacement. A for-of body costs more bytecode than the DFG inlines at a call
//   (maximumFunctionForCallInlineCandidateBytecodeCostForDFG, 80), so both runs raise that cap to the FTL's default
//   of 170, which lazyCallee, at 145, fits. Only the DFG's inlining decisions read the option, so it is free.
// No native exit records a tmp key (N7), so the unit tests U2 and U3 cover tmp keys.
// The Producer commits both CBs with strict on, and in twins builds checks their lazy-operand records; the Consumer
// installs them, passes their twins, and in twins builds checks that they were imported.
(function main(role) {
    class Countdown {
        constructor(count) {
            this.count = count;
        }
        [Symbol.iterator]() {
            return this;
        }
        next() {
            if (this.count <= 0)
                return { done: true, value: undefined };
            const value = this.count--;
            return { done: false, value };
        }
    }

    function lazyCallee(iterable, unit) {
        let total = 0;
        for (const value of iterable)
            total += value * unit.length;
        return total;
    }
    // lazyCallee's numCalleeLocals(): the callee-register count of its bytecode dump (--dumpGeneratedBytecodes=true).
    // x86_64 and ARM64 reserve the same four callee-save locals in every frame, so both targets give this count.
    const lazyCalleeCalleeLocals = 22;

    function lazyCaller(map, set, iterable, unit) {
        let total = 0;
        for (const entry of map)
            total += entry[1];
        for (const item of set)
            total += item;
        // Each of these bindings holds a register of lazyCaller's frame until after the call, which pushes the call's
        // argument area, where the inlined lazyCallee reads `unit`, well past lazyCalleeCalleeLocals.
        const pad0 = 0, pad1 = 0, pad2 = 0, pad3 = 0, pad4 = 0, pad5 = 0, pad6 = 0, pad7 = 0;
        const pad8 = 0, pad9 = 0, pad10 = 0, pad11 = 0, pad12 = 0, pad13 = 0, pad14 = 0, pad15 = 0;
        const pad16 = 0, pad17 = 0, pad18 = 0, pad19 = 0, pad20 = 0, pad21 = 0, pad22 = 0, pad23 = 0;
        total += lazyCallee(iterable, unit);
        return total + pad0 + pad1 + pad2 + pad3 + pad4 + pad5 + pad6 + pad7 + pad8 + pad9 + pad10 + pad11
            + pad12 + pad13 + pad14 + pad15 + pad16 + pad17 + pad18 + pad19 + pad20 + pad21 + pad22 + pad23;
    }
    function lazyRoot(holder, rounds) {
        const word = holder.word;
        let total = 0;
        for (let i = 0; i < rounds; ++i)
            total += word.length;
        return total;
    }
    noInline(lazyCaller);
    noInline(lazyRoot);

    // Only the order of tier-ups keeps lazyCallee out of the DFG, since operationOptimize clears ShouldAlwaysBeInlined
    // at any entry, from the prologue as from a loop. Each call advances lazyCaller's counter by a walk over every map
    // and set entry and lazyCallee's by one entry and a two-step loop, so lazyCaller crosses its thresholds in far
    // fewer calls. Its DFG compile waits for its profiles, though: its walks take the fast map and set paths, so the
    // value profiles of the generic iteration and iterator-close paths stay empty, its profile liveness stays below
    // desiredProfileLivenessRate, and shouldOptimizeNowFromBaseline defers the compile maximumOptimizationDelay times,
    // re-arming the warm-up threshold each time. lazyCallee leaves the LLInt during those deferrals, so the inlinee's
    // exits land in its baseline code, which delta captures. lazyCaller then compiles, with lazyCallee inlined, while
    // lazyCallee's baseline counter is still far from its threshold, and from then on lazyCallee's baseline code runs
    // only after an exit inside the inlinee.
    const map = new Map();
    const set = new Set();
    for (let i = 0; i < 128; ++i) {
        map.set(i, i);
        set.add(i);
    }
    const unit = "ab";
    const steady = { word: "abc" };

    if (role === "Producer") {
        for (let i = 0; i < 100000 && !numberOfDFGCompiles(lazyCaller); ++i)
            lazyCaller(map, set, new Countdown(2), unit);
        if (!numberOfDFGCompiles(lazyCaller) || numberOfDFGCompiles(lazyCallee))
            throw new Error(`lazyCaller has ${numberOfDFGCompiles(lazyCaller)} DFG compilations and lazyCallee ${numberOfDFGCompiles(lazyCallee)}; the callee must run only inlined`);
        // An array where `unit` held strings.
        for (let i = 0; i < 4; ++i)
            lazyCaller(map, set, new Countdown(2), [i, i, i]);

        for (let i = 0; i < 100000 && !numberOfDFGCompiles(lazyRoot); ++i)
            lazyRoot(steady, 3);
        for (let i = 0; i < 100000 && !reoptimizationRetryCount(lazyRoot); ++i)
            lazyRoot({ word: [i, i] }, 3);
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
            // A record's local index is -value - 1 (VirtualRegister::toLocal).
            const callee = sections.checkBody(lazyCallee, "call", "lazyCallee").state;
            if (!callee.lazyOperands.some(record => record.kind === OperandKind.Local && -record.value - 1 >= lazyCalleeCalleeLocals))
                throw new Error(`lazyCallee: no lazy-operand record names a local past its ${lazyCalleeCalleeLocals} callee locals: ${sections.describe(callee.lazyOperands)}`);
        }
    }

    // Every role: the bodies run hot with the shapes of both phases, so the Consumer's own DFG compilations parse the
    // imported lazy-operand predictions.
    let total = 0;
    for (let i = 0; i < 60; ++i) {
        const otherUnit = i % 10 === 8 ? [i, i, i] : unit;
        total += lazyCaller(map, set, new Countdown(2), otherUnit);
        total += lazyRoot(i % 10 === 7 ? { word: [i, i] } : steady, 3);
    }
    print(total);

    if (role === "Consumer" && typeof jitcacheBodyEvents === "function") {
        const sections = load("./resources/cb-sections.js", "caller relative");
        sections.expectImported(lazyCaller, "call", "lazyCaller");
        sections.expectImported(lazyCallee, "call", "lazyCallee");
        sections.expectImported(lazyRoot, "call", "lazyRoot");
    }
})(...arguments);

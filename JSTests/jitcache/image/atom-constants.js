// jitcache-requires: twins
// jitcache-runs: Off --diskCachePath=.; Producer --jitcache-delta-at-exit --useConcurrentJIT=false --diskCachePath=.; Consumer --useConcurrentJIT=false --useEagerCodeBlockJettisonTiming=true --diskCachePath=.
// jitcache-runs: Off --diskCachePath=.; Producer --jitcache-delta-at-exit --useConcurrentJIT=false --diskCachePath=.; Consumer --useConcurrentJIT=false --diskCachePath=.
// jitcache-pin: off its runs decode the jsc shell's bytecode cache, whose payload computeJSCBytecodeCacheVersion ties to this build, so the pin's build rejects it and compiles strict equalities against generated atoms where this build compiles them against decoded plain strings; and with the first consumer's options its Off run polls jitcacheBodyEvents, a function only this build has, until the plain functions' CodeBlocks age out, which the comparison's --useGC=false never lets them do

// SPEC-image.md T18 (N25, section 11.1, I22). Every run works in its sequence's scratch directory, so --diskCachePath=.
// shares one bytecode cache per sequence: the first run generates this script and writes the cache, and the producer and
// the consumer decode it. The shell caches each function as an update of its own, whose strings decode as fresh plain
// strings, so every string constant longer than an inline string stays plain until a property-key use makes it an atom
// in place. Each function compares its own long constant with === or !==, as a value or as a branch, on the left or the
// right, and returns the constant itself when asked. The constants on the left are template literals without
// substitutions: StrictEqualNode::emitBytecode and BinaryOpNode::emitBytecode move a string literal on the left of
// === or !== to the right, while a template literal, which is no StringNode, stays its instruction's left operand and
// shares the constant register of the string literal its function returns.
// - The producer makes the "early" constants atoms before the functions compile, so their templates compare by atom; the
//   "late" ones after they compiled and before its delta at exit, which recaptures them with an atom map that marks
//   those constants while their images keep the generic comparison; the "plain" ones stay plain.
// - Each consumer installs every function at its first call. The UCB lane atomizes the marked constants first, so the
//   late functions hold atoms where their images compare generically. The consumer checks the imports, the operand each
//   image recorded at its strict equality (twin data, kind 8), that only the early functions' images carry a
//   UCBConstantAtom pointer (C6, C7; T1), and that every constant the capture marked is an atom while the plain ones are
//   not.
// - A plain constant exists only once its function's UCB is decoded at that first call, which installs the image too, so
//   the first sequence's consumer makes the plain constants atoms after their first install: it uses each as a property
//   key, ages the two CodeBlocks out with eager jettison timing, which empties their UCBs' sharing slots, and calls the
//   functions again. The UCB lane attaches the same body to each live UCB, and the newborn CodeBlock installs the image
//   over atoms the capture never marked. The second consumer calls the functions in reverse order.
// Every install passes the twin check, whose twin compile answers each strict equality from the producer's recorded
// operand whatever atoms the consumer holds, and under strict W4 rejects a recorded atom operand that is no atom here.
// The Off runs take the consumer's path and its native steps, the wait for the plain functions' code to age out included
// where their options age code; only the consumer reads what JITCache imported, installed or attached.
(function main(role, scratch, artifact, sequence) {
    function valueRight(x, constant) {
        if (constant)
            return "jitcache-atom-constant-early-value-right";
        return x === "jitcache-atom-constant-early-value-right";
    }
    function branchLeft(x, constant) {
        if (constant)
            return "jitcache-atom-constant-early-branch-left";
        if (`jitcache-atom-constant-early-branch-left` !== x)
            return false;
        return true;
    }
    function valueLeft(x, constant) {
        if (constant)
            return "jitcache-atom-constant-late-value-left";
        return `jitcache-atom-constant-late-value-left` !== x;
    }
    function branchRight(x, constant) {
        if (constant)
            return "jitcache-atom-constant-late-branch-right";
        if (x === "jitcache-atom-constant-late-branch-right")
            return true;
        return false;
    }
    function plainValue(x, constant) {
        if (constant)
            return "jitcache-atom-constant-plain-value";
        return x === "jitcache-atom-constant-plain-value";
    }
    function plainBranch(x, constant) {
        if (constant)
            return "jitcache-atom-constant-plain-branch";
        if (`jitcache-atom-constant-plain-branch` === x)
            return true;
        return false;
    }
    const early = [valueRight, branchLeft];
    const late = [valueLeft, branchRight];
    const plain = [plainValue, plainBranch];
    const all = [...early, ...late, ...plain];

    const keys = { };
    const useAsKeys = group => {
        for (const fn of group)
            keys[fn(null, true)] = true;
    };
    const expectAtoms = group => {
        for (const fn of group) {
            if (!$vm.isAtomString(fn(null, true)))
                throw new Error(`${fn.name}'s constant is no atom after its use as a property key`);
        }
    };
    const countOf = fn => {
        const constant = fn(null, true);
        let count = 0;
        for (let i = 0; i < 40; ++i) {
            const input = [constant, constant.slice(0, 9) + constant.slice(9), "jitcache-other", i][i % 4];
            count += fn(input, false) ? 1 : 0;
        }
        return count;
    };

    const producing = role === "Producer";
    if (producing) {
        for (const fn of all) {
            if ($vm.isAtomString(fn(null, true)))
                throw new Error(`${fn.name}'s constant is an atom before any use, so this run did not decode it`);
        }
        useAsKeys(early);
    }

    const order = [...all];
    if (!producing && sequence === "1")
        order.reverse();
    const counts = new Map();
    for (const fn of order)
        counts.set(fn, countOf(fn));
    if (producing) {
        useAsKeys(late);
        expectAtoms([...early, ...late]);
    }
    print(all.map(fn => `${fn.name} ${counts.get(fn)}`).join(", "));
    if (producing)
        return;

    // What JITCache imported: in a consumer that determines its imports, each function ran its import, and its image
    // recorded at its one strict equality the operand its template compared by atom: the early constants' side, and
    // none for the late and plain ones, which were plain when it compiled, so only the early images carry a
    // UCBConstantAtom pointer. The import made atoms of exactly the constants the capture marked, the late ones only
    // because the producer's delta recaptured their bodies after it made them atoms. Each check first calls its function,
    // which keeps its CodeBlock from aging out under the first consumer's eager timing; a CodeBlock that aged out earlier
    // came back by an attach, and the counts the import check reads belong to the UCB, so they still show any LLInt run.
    const sections = role === "Consumer" ? load("./resources/image-sections.js", "caller relative") : null;
    const readsImports = !!sections && sections.checksImports(role);
    if (readsImports) {
        const operand = sections.strictEqualityAtomOperand;
        const recordedOperands = new Map([
            [valueRight, operand.Rhs], [branchLeft, operand.Lhs], [valueLeft, operand.None], [branchRight, operand.None],
            [plainValue, operand.None], [plainBranch, operand.None],
        ]);
        for (const [fn, expected] of recordedOperands) {
            const constant = fn(null, true);
            sections.expectImported(fn, "call", fn.name);
            const key = jitcacheBodyKey(fn, "call");
            const twins = key ? jitcacheReadSection(key, "image-twins.baseline") : null;
            if (!twins)
                throw new Error(`${fn.name}'s key ${key} holds no image-twins.baseline`);
            const recorded = sections.parseTwinsSection(twins).inputs
                .filter(input => input.kind === sections.twinCompileInputKind.StrictEqualityAtomOperand).map(input => input.value);
            if (recorded.length !== 1 || recorded[0] !== expected)
                throw new Error(`${fn.name}'s image records the strict-equality operands [${recorded}], not [${expected}]`);
            const comparesByAtom = sections.bodyTargets(fn, "call", fn.name).fixups.some(fixup => fixup.kind === sections.imageTargetKind.UCBConstantAtom);
            if (comparesByAtom !== early.includes(fn))
                throw new Error(`${fn.name}'s image ${comparesByAtom ? "carries" : "lacks"} a UCBConstantAtom pointer`);
            const marked = !plain.includes(fn);
            if ($vm.isAtomString(constant) !== marked)
                throw new Error(`${fn.name}'s constant is ${marked ? "no atom, though the capture marked it" : "an atom that nothing in this run made"}`);
        }
    }
    if (sequence !== "0")
        return;

    // The first sequence's consumer path: the plain constants become atoms after their first install, and the plain
    // functions run again once their code aged out, with the counts they gave before.
    const before = readsImports ? { attaches: jitcacheUCBStatistics().attaches, installs: jitcacheProgress().installs } : null;
    useAsKeys(plain);
    expectAtoms(plain);
    // Each plain function's counter moved since its install, so the first aging check renews its lease and a later one
    // jettisons it, which leaves its executable without a CodeBlock. A run whose options do not age code takes the steps
    // that follow without the wait.
    if (jscOptions().useEagerCodeBlockJettisonTiming) {
        for (let attempt = 0; plain.some(fn => jitcacheBodyEvents(fn, "call") !== null); ++attempt) {
            if (attempt === 250)
                throw new Error("a plain function's CodeBlock never aged out");
            sleepSeconds(0.02);
            fullGC();
        }
    }
    for (const fn of plain) {
        const count = countOf(fn);
        if (count !== counts.get(fn))
            throw new Error(`${fn.name} counted ${count} when it ran again, not ${counts.get(fn)}`);
    }
    if (readsImports) {
        sections.expectImports(role, { plainValue, plainBranch });
        const attaches = jitcacheUCBStatistics().attaches - before.attaches;
        const installs = jitcacheProgress().installs - before.installs;
        if (attaches < plain.length || installs < plain.length)
            throw new Error(`the plain functions' next calls made ${attaches} attaches and ${installs} installs, not one of each per function`);
    }
})(...arguments);

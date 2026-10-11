// jitcache-runs: Off --useBaselineJIT=false
// jitcache-check: 0 resources/llint-counts.ts

// Harness sub-SPEC H6, with the baseline JIT off: a function without branches gains, at each of three calls, as many
// llintInstructions as its bytecode has instructions, and so does one that calls eval directly; a function whose loop
// runs n times gains, over n = 0, 1 and 2, amounts in arithmetic progression; and every other count of the three stays
// 0. The script prints each function's gains and other counts as JSON lines, and dumps the first two functions'
// bytecode to stderr, where the checker counts their instructions. No oracle compares an Off-only sequence, so the
// counts may be printed.
(function main() {
    function h6Straight(a, b) {
        const sum = a + b;
        return sum * 2;
    }
    function h6DirectEval(source) {
        return eval(source);
    }
    function h6Loop(n) {
        let total = 0;
        for (let i = 0; i < n; ++i)
            total += i;
        return total;
    }

    // Before its first call a function has no CodeBlock, and its body has executed nothing.
    const llintInstructions = fn => jitcacheBodyEvents(fn, "call")?.llintInstructions ?? 0;
    function measure(fn, calls) {
        const gains = calls.map(call => {
            const before = llintInstructions(fn);
            call();
            return llintInstructions(fn) - before;
        });
        const { llintInstructions: ignored, ...others } = jitcacheBodyEvents(fn, "call");
        return { gains, others };
    }
    const results = {
        h6Straight: measure(h6Straight, [() => h6Straight(1, 2), () => h6Straight(3, 4), () => h6Straight(5, 6)]),
        h6DirectEval: measure(h6DirectEval, [() => h6DirectEval("1 + 1"), () => h6DirectEval("2 + 2"), () => h6DirectEval("3 + 3")]),
        h6Loop: measure(h6Loop, [() => h6Loop(0), () => h6Loop(1), () => h6Loop(2)]),
    };
    for (const [name, result] of Object.entries(results))
        print(JSON.stringify({ name, ...result }));
    $vm.dumpBytecodeFor(h6Straight);
    $vm.dumpBytecodeFor(h6DirectEval);
})();

// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; ConsumerProducer

// SPEC-ucb.md section 13.3, context-on-demand.js: a context or holder digest is computed only where something reads it
// (section 3.4; THREAD's "no work ahead of demand").
// - Each root body the Producer captured, a Function-constructor body and a JSC builtin, computes its holder digest once,
//   at its import (statistics holderDigests), and nothing more at later calls.
// - A part only the second run executes holds a program, a new Function body, the first call of a JSC builtin and
//   direct evals of fresh strings under a lexical scope with a thousand bindings, none with a body in the artifact.
//   Across it the Consumer computes no context and no holder digest; the ConsumerProducer computes one context per direct
//   eval, when it records the eval's UCB, and nothing else.
// The part prints nothing and leaves nothing behind, so the Producer, which skips it, prints what its oracle prints.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);

    function measured(label, action, expectations)
    {
        const before = t.statistics();
        const value = action();
        const after = t.statistics();
        if (t.imports)
            t.expect(label, t.delta(before, after), expectations);
        return value;
    }

    const constructed = new Function("x", "let total = x; for (let i = 0; i < 20; ++i) total = (total * 3 + i) | 0; return total;");
    let constructedTotal = measured("the Function-constructor body's import", () => constructed(1), { imports: 1, holderDigests: 1, contextDigests: 1 });
    constructedTotal = (constructedTotal + measured("the Function-constructor body's second call", () => constructed(2), { holderDigests: 0, contextDigests: 0 })) | 0;
    const values = [10, 20, 30, 40];
    let at = measured("Array.prototype.at's import", () => values.at(1), { imports: 1, holderDigests: 1, contextDigests: 1 });
    at += measured("Array.prototype.at's second call", () => values.at(-1), { holderDigests: 0, contextDigests: 0 });
    for (let i = 0; i < 400; ++i) {
        constructedTotal = (constructedTotal + constructed(i)) | 0;
        at += values.at(i & 3);
    }
    print(`roots ${constructedTotal} ${at}`);

    if (t.role === "Producer") {
        t.finish();
        return;
    }

    const directEvals = 5;
    const lexicalBindings = Array.from({ length: 1000 }, (_, i) => `let codBinding${i} = ${i};`);
    const before = t.statistics();
    loadString("{ let fresh = 41; fresh + 1; }");
    const scoped = new Function("count", [
        ...lexicalBindings,
        "let total = 0;",
        "for (let i = 0; i < count; ++i)",
        "    total += eval('codBinding' + i + ' + ' + i);",
        "return total;",
    ].join("\n"));
    const evaluated = scoped(directEvals);
    const found = [3, 1, 4, 1, 5].findLastIndex(value => value === 1);
    const after = t.statistics();
    t.check(evaluated === 20 && found === 3, `the part computed ${evaluated} and ${found}`);
    if (t.role === "ConsumerProducer")
        t.expect("the part", t.delta(before, after), { contextDigests: directEvals, holderDigests: 0, imports: 0 });
    else if (t.role === "Consumer")
        t.expect("the part", t.delta(before, after), { contextDigests: 0, holderDigests: 0, imports: 0 });
    t.finish();
})(...arguments);

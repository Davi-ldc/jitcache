// SPEC-ucb.md section 13.3, regexp-constants.js, F22 and SPEC-ucb.codec.md E11: a RegExp constant is the cell the VM's
// RegExpCache holds for its pattern and flags, shared with every live RegExp object of that pattern and flags, and code
// deletion clears that cell's compiled state. The script keeps live RegExp objects whose patterns and flags the
// functions below also write as literals, runs them, and asks for all code to be deleted once the VM is idle
// ($vm.deleteAllCodeWhenIdle(), which runs when the script's evaluation returns). The functions' bodies are generated,
// or imported in the Consumer, only afterwards, in a timer: so in the Producer generation takes cells whose code was
// cleared and the capture encodes them, and in the Consumer the import's decode returns those cells in the same state.
// With strict on no import is invalid material (C5 holds of a cleared cell), T1 and T6 hold at every import (section
// 13.2), and the results equal the JITCache-off run. The Bun variant, which clears the cells with Bun.shrink(), is
// regexp-constants-bun.js.
//
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4). A plain build has no $vm, so there
// nothing is deleted and every run still behaves alike.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const rounds = 400;

    // The live objects hold the same pattern and flags as the literals of the first four functions; the last literal has
    // none.
    const live = [new RegExp("a(b+)c", "g"), new RegExp("^[a-z]+-\\d+$", "i"), new RegExp("(?<word>\\w+)\\s", "u"), new RegExp("x.y", "sy")];
    let liveTotal = 0;
    for (const regExp of live) {
        regExp.lastIndex = 0;
        liveTotal += String(regExp.exec("abbbc ABC-12 word x\ny")).length;
    }
    if (typeof $vm === "object" && $vm && typeof $vm.deleteAllCodeWhenIdle === "function")
        $vm.deleteAllCodeWhenIdle();

    function captureGroup(s)
    {
        const match = /a(b+)c/g.exec(s);
        return match ? match[1].length : -1;
    }
    function caseless(s)
    {
        return /^[a-z]+-\d+$/i.test(s);
    }
    function namedGroup(s)
    {
        const match = /(?<word>\w+)\s/u.exec(s);
        return match ? match.groups.word : null;
    }
    function stickyDotAll(s)
    {
        const regExp = /x.y/sy;
        regExp.lastIndex = s.indexOf("x");
        return regExp.test(s);
    }
    function ownPattern(s)
    {
        return s.replace(/o+/g, "0");
    }

    const inputs = ["abbbc", "zzac", "ABC-12", "abc-x", "word rest", "!", "x\ny", "xzy", "foo boo"];
    setTimeout(() => {
        const lines = [`live ${liveTotal}`];
        for (const fn of [captureGroup, caseless, namedGroup, stickyDotAll, ownPattern]) {
            const before = t.statistics();
            const first = fn(inputs[0]);
            const after = t.statistics();
            if (t.imports)
                t.expect(`${fn.name}'s first call`, t.delta(before, after), { imports: [">=", 1] });
            const results = [String(first)];
            for (let round = 0; round < rounds; ++round) {
                const result = fn(inputs[round % inputs.length]);
                if (round < inputs.length)
                    results.push(String(result));
            }
            lines.push(`${fn.name}: ${results.join(" ")}`);
        }
        t.check(t.status() === null || t.status() === undefined, () => `cache activity turned off at ${t.status()}`);
        for (const line of lines)
            print(line);
        live.length = 0;
        t.finish();
    }, 0);
})(...arguments);

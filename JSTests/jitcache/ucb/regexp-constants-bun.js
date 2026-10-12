// jitcache-host: bun

// SPEC-ucb.md section 13.3, regexp-constants.js, the Bun variant: the same as regexp-constants.js, with the RegExp cells'
// compiled state cleared by Bun.shrink(), which asks the VM to delete all code once it is idle
// (VM::shrinkFootprintWhenIdle, F22): live RegExp objects hold the patterns and flags of the literals below, they run,
// Bun.shrink() clears their cells when the module's evaluation returns, and the functions' bodies are generated, or
// imported in the Consumer, only afterwards, in a timer. With strict on no import is invalid material, T1 and T6 hold at
// every import (section 13.2), and the output equals the JITCache-off run.
//
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4); Bun calls delta at exit for the
// Producer.
const { context } = require("./resources/ucb-bun.js");

const t = context();
const rounds = 400;

const live = [new RegExp("a(b+)c", "g"), new RegExp("^[a-z]+-\\d+$", "i"), new RegExp("(?<word>\\w+)\\s", "u"), new RegExp("x.y", "sy")];
let liveTotal = 0;
for (const regExp of live) {
    regExp.lastIndex = 0;
    liveTotal += String(regExp.exec("abbbc ABC-12 word x\ny")).length;
}
Bun.shrink();

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
        console.log(line);
    live.length = 0;
    t.finish();
}, 0);

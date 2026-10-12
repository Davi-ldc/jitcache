// The app of builtin-roots.js, built with `bun build --compile --bytecode`: a standalone executable decodes Bun's
// internal modules with decodeBuiltinFunction from the bytecode it embeds (generateInternalModule), and each decoded
// module is a builtin root whose digest comes from its provider (SPEC-ucb.md sections 7.2.4 and 7.2.6). The app loads
// node:querystring, whose root gets one identity and takes its source digest from its provider (statistics: a root, a
// supplied digest, no digest computed from text), and runs its functions until they are captured. As a Consumer each
// of them imports or is seeded at its first call: from the app's own captures over the app's artifact, and from the
// captures builtin-roots.js made of the module it created when the app runs over that script's artifact. The first
// calls go through functions that run once, so that no body of the app, which its Producer ran hot, imports or is
// seeded inside a window: only the module's bodies can.
const { appContext } = require("./ucb-bun.js");

const t = appContext();

const beforeLoad = t.statistics();
const querystring = require("node:querystring");
const afterLoad = t.statistics();
if (t.configured) {
    t.expect("loading node:querystring", t.delta(beforeLoad, afterLoad), {
        "counts.roots": [">=", 1], suppliedSourceDigests: [">=", 1], sourceDigests: 0,
    });
}

const inputs = ["a=1&b=two", "x=%20y&x=z", "plain", "k=v&k=w&q"];
const calls = {
    escape: s => querystring.escape(s),
    parse: s => JSON.stringify(querystring.parse(s)),
};
const firstCalls = {
    escape: () => querystring.escape(inputs[0]),
    parse: () => JSON.stringify(querystring.parse(inputs[0])),
};
const lines = [];
for (const name of ["escape", "parse"]) {
    const before = t.statistics();
    const first = firstCalls[name]();
    const after = t.statistics();
    const delta = t.delta(before, after);
    if (t.imports && delta)
        t.check(delta.imports + delta.seededDecodes >= 1, () => `querystring.${name}'s first call neither imported nor was seeded: ${JSON.stringify(delta)}`);
    let total = first.length;
    for (let i = 0; i < 400; ++i)
        total = (total + calls[name](inputs[i % inputs.length]).length) | 0;
    lines.push(`querystring.${name} ${first} ${total}`);
}
console.log(lines.join("\n"));
t.finish();

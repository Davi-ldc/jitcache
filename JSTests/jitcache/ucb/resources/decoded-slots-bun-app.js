// The app of decoded-slots-bun.js, built with `bun build --compile --bytecode` together with
// decoded-slots-bun-module.js, which it requires at run time. Bun decodes their functions lazily from the bytecode the
// executable embeds, which holds one body per function, the call body of an ordinary function (eager generation for
// bytecode caches, SPEC-ucb.md F2). The Consumer's checks, which the comments below state, are those of
// decoded-slots.js for this cache (SPEC-ucb.md section 13.3).
const path = require("node:path");
const { appContext } = require("./ucb-bun.js");

const t = appContext();

function dbA(x)
{
    function dbChild(y)
    {
        return y + 1;
    }
    return dbChild(x) * 2;
}

function dbB(x)
{
    if (new.target) {
        this.tag = "ab";
        this.value = x;
        return;
    }
    return x * 3 + "ab".length;
}

// The module holds dbC, whose long string constant no other string of the executable shares (see the module), so the
// object keys below are built at run time (longKey). It sits beside the entry module in the executable, whose path is
// Bun.main. A bundle inlines `__dirname` as the directory the app was built from, so path.join(__dirname, ...) would load
// the module's source file from disk, without its bytecode.
const modulePath = path.join(path.dirname(Bun.main), "decoded-slots-bun-module.js");
const longKey = ["decoded", "slots", "bun", "long", "key"].join("-");
const lines = [];
const consumer = t.imports;

// The requested slot of a lazily decoded function is seeded at its first call; its child keeps its lazily cached slot
// and is seeded at its own first call, inside this one.
lines.push(t.window("dbA's first call", () => dbA(1), consumer ? { seededDecodes: 2 } : { }));

// `new` first on an ordinary function whose call body alone was cached: the decode records the call slot as decoded
// without settling the request, and the empty construct slot imports.
lines.push(t.window("dbB's first construction", () => new dbB(2), consumer ? { "records.Decoded": [">=", 1], imports: 1, seededDecodes: 0 } : { }).value);
// The decoded call slot meets its own first request live; its only string constant is an inline string, which decodes as
// an atom, so it attaches.
lines.push(t.window("dbB's first call", () => dbB(1), consumer ? { attaches: 1 } : { }));

// dbC's construction decodes its call slot, whose long string constant decodes as a plain string while the body's atom
// map marks it, so the call slot's first request misses AtomMap without a stamp. That call then reads o[key], whose
// property key makes the constant's JSString an atom in place (F19).
const first = require(modulePath);
lines.push(t.window("dbC's first construction", () => new first.dbC(3), consumer ? { "records.Decoded": [">=", 1] } : { }).value);
lines.push(t.window("dbC's first call", () => first.dbC("x", { [longKey]: 5 }), consumer ? { attaches: 0, "misses.AtomMap": 1 } : { }));

// The attach needs a later request for that same UCB, which deleting code cannot make: this cache holds one body per
// function, so the construction filled dbC's construct slot beside the decoded call slot, and publishing it put the UFE
// in the clearable set (F1, section 7.5), whose slots Bun.shrink() clears, the decoded call slot included. The
// module's second evaluation makes one, as the second realm of decoded-slots.js does: it runs on the program the
// CodeCache kept from the first, whose functions become new FunctionExecutables of the same UFEs, so its dbC's first
// call is a new CodeBlock's request for the live decoded slot. It attaches now that the marked constant is an atom, at
// the commit identifier it matched at, computing no digest (section 7.3.4 step 5). Until then the call slot runs only
// its first call, since baseline code parked on its UCB would serve the new CodeBlock instead (step 2).
delete require.cache[require.resolve(modulePath)];
const second = require(modulePath);
t.check(second.dbC !== first.dbC, "requiring the module again did not evaluate it again");
lines.push(t.window("dbC's call in the module's second evaluation", () => second.dbC(4, { [longKey]: 6 }), consumer ? {
    attaches: 1, contextDigests: 0, holderDigests: 0, sourceDigests: 0,
} : { }));

let total = 0;
for (let i = 0; i < 400; ++i) {
    total = (total + dbA(i) + dbB(i) + new dbB(i).value) | 0;
    total = (total + second.dbC(i, { [longKey]: i }) + new second.dbC(i).value + (second.dbC(longKey) ? 1 : 0)) | 0;
}
lines.push(`total ${total}`);
console.log(lines.join("\n"));
t.finish();

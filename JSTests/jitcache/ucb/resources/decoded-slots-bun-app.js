// The app of decoded-slots-bun.js, built with `bun build --compile --bytecode`. Bun decodes its functions lazily from
// the bytecode the executable embeds, which holds one body per function, the call body of an ordinary function
// (eager generation for bytecode caches, SPEC-ucb.md F2). The Consumer's checks, which the comments below state, are
// those of decoded-slots.js for this cache (SPEC-ucb.md section 13.3).
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

// dbC's call body holds the one string constant of the app with these characters. A `--compile --bytecode` build keeps
// every string of four or more characters in one table that all its chunks share, and decodes an identifier's entry as
// an atom, which a constant with the same characters then shares (DecoderStringTable::atomFor and jsStringFor in
// runtime/CachedTypes.cpp). So no identifier or other constant of the app holds them: o[key] reads through the
// constant's own value (get_by_val), where o["..."] would compile to get_by_id with an identifier of its own, and the
// object keys below are built at run time (longKey). `key` is a let, which Bun's transpiler never inlines, where it
// inlines a const whose value is a literal whenever it minifies syntax.
function dbC(x, o)
{
    if (new.target) {
        this.value = x;
        return;
    }
    let key = "decoded-slots-bun-long-key";
    if (o)
        return o[key];
    return x === key;
}

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
lines.push(t.window("dbC's first construction", () => new dbC(3), consumer ? { "records.Decoded": [">=", 1] } : { }).value);
lines.push(t.window("dbC's first call", () => dbC("x", { [longKey]: 5 }), consumer ? { attaches: 0, "misses.AtomMap": 1 } : { }));

let total = 0;
for (let i = 0; i < 400; ++i) {
    total = (total + dbA(i) + dbB(i) + new dbB(i).value) | 0;
    total = (total + dbC(i, { [longKey]: i }) + new dbC(i).value + (dbC(longKey) ? 1 : 0)) | 0;
}
lines.push(`total ${total}`);

// Deleting all code leaves the decoded slots in their UFEs, which Bun's decode never adds to the clearable set (F1), and
// takes each function's CodeBlock, so dbC's next call is a later request for the same live UCB: it attaches now that its
// marked constant is an atom, at the commit identifier it matched at, computing no digest (section 7.3.4 step 5).
Bun.shrink();
setTimeout(() => {
    lines.push(t.window("dbC's call after its constant became an atom", () => dbC(4, { [longKey]: 6 }), consumer ? {
        attaches: 1, contextDigests: 0, holderDigests: 0, sourceDigests: 0,
    } : { }));
    console.log(lines.join("\n"));
    t.finish();
}, 0);

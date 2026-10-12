// The app of borrowed-bytecode.js, built with `bun build --compile --bytecode`. Bun decodes it from the persistent
// payload the executable embeds, so its UCBs borrow their instruction streams from the executable's section
// (useBorrowedBytecodeFromCache). In the Producer its functions run until they are captured, which encodes the borrowed
// streams (SPEC-ucb.codec.md E3); in the Consumer each decoded function is seeded at its first call (statistics).
const { appContext } = require("./ucb-bun.js");

const t = appContext();

function bbSum(n)
{
    let total = 0;
    for (let i = 0; i < n; ++i)
        total = (total + i * 3) | 0;
    return total;
}

function bbLabel(x)
{
    return x % 2 ? "borrowed-bytecode-odd" : "borrowed-bytecode-even";
}

class BbPoint {
    constructor(x)
    {
        this.x = x;
    }
    scaled(k)
    {
        return this.x * k;
    }
}

const lines = [];
for (const [name, call] of [["bbSum", i => bbSum(i)], ["bbLabel", i => bbLabel(i)], ["BbPoint", i => new BbPoint(i).scaled(3)]]) {
    const first = t.window(`${name}'s first call`, () => call(1), t.imports ? { seededDecodes: [">=", 1] } : { });
    let total = String(first).length;
    for (let i = 0; i < 400; ++i)
        total = (total + String(call(i)).length) | 0;
    lines.push(`${name} ${first} ${total}`);
}
console.log(lines.join("\n"));
t.finish();

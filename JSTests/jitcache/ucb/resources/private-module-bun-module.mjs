// The ES module of private-module-bun.js, which the host imports and a Bun.ModuleGraph with `globals` imports again.
// The graph's copy resolves pmbBase in the graph's globals, where the host's finds no such global, so the two link one
// UCB's code under different scope chains.
export function pmbRead(x)
{
    return (x * 3 + (typeof pmbBase === "number" ? pmbBase : 1)) | 0;
}

let pmbTotal = 0;
for (let i = 0; i < 100; ++i)
    pmbTotal = (pmbTotal + pmbRead(i)) | 0;
export const pmbStart = pmbTotal;

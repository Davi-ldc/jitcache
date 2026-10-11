// The module of body-kinds.js. Its top level loops long enough to compile the module body to baseline through loop OSR,
// and it exports a function nested in it, whose key names the module body as its parent.
export function moduleWeigh(values) {
    let total = 0;
    for (const value of values)
        total += value * 3;
    return total;
}

let total = 0;
for (let i = 0; i < 1500; ++i)
    total += (i & 7) * 2;
export const moduleTotal = total;

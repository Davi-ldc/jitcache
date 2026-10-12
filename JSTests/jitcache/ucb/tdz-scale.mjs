// SPEC-ucb.md section 13.3, tdz-scale.mjs, F26 and I25: what a body costs does not grow with the names its enclosing
// scopes declare. The test writes two modules into the sequence's scratch directory and imports them: one with 5,000
// top-level bindings, 4,000 consts and 1,000 imports, and one with ten. Each starts with the same 200 small exported
// function declarations, each holding a nested closure, followed by a function that wraps the same bindings as consts
// around the same 200 declarations, as Bun wraps a CommonJS module, so that the module's TDZ link and the wrapper's name
// every binding. Both start at the same offsets in both modules, so their bodies differ only in their enclosing scopes.
// - In the Consumer every function and closure imports (statistics, imports), and tdzEnvironmentDigests grows by the few
//   distinct environments the holder digests reach, not by the number of functions.
// - In twins runs each small function's and closure's ucb.core, read with jitcacheReadSection, has the size it has in
//   the module of ten bindings; T1 and T5 hold at every import (section 13.2).
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4).
load("./resources/ucb.js", "caller relative");

await (async function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const functionCount = 200;
    const rounds = 80;

    function moduleText(bindings, constCount, importsName)
    {
        const names = Array.from({ length: bindings }, (_, i) => `tdzBinding${i}`);
        return [
            ...Array.from({ length: functionCount }, (_, k) => `export function tdzF${k}(x) { return () => x + ${k}; }`),
            "export function tdzWrapper() {",
            ...Array.from({ length: functionCount }, (_, k) => `    function tdzW${k}(x) { return () => x * ${k}; }`),
            ...names.map((name, i) => `    const ${name} = ${i};`),
            `    return [${Array.from({ length: functionCount }, (_, k) => `tdzW${k}`).join(", ")}];`,
            "}",
            `import { ${names.slice(constCount).join(", ")} } from "./${importsName}";`,
            ...names.slice(0, constCount).map((name, i) => `const ${name} = ${i};`),
            `export const tdzBindingCount = ${bindings};`,
        ].join("\n");
    }
    function importsText(from, to)
    {
        return Array.from({ length: to - from }, (_, i) => `export const tdzBinding${from + i} = ${from + i};`).join("\n");
    }
    writeFile(`${scratch}/tdz-scale-large-imports.mjs`, importsText(4000, 5000));
    writeFile(`${scratch}/tdz-scale-large.mjs`, moduleText(5000, 4000, "tdz-scale-large-imports.mjs"));
    writeFile(`${scratch}/tdz-scale-small-imports.mjs`, importsText(5, 10));
    writeFile(`${scratch}/tdz-scale-small.mjs`, moduleText(10, 5, "tdz-scale-small-imports.mjs"));

    const modules = [await import(`${scratch}/tdz-scale-large.mjs`), await import(`${scratch}/tdz-scale-small.mjs`)];
    const bodies = [];
    const before = t.statistics();
    for (const module of modules) {
        const functions = [];
        const closures = [];
        for (let k = 0; k < functionCount; ++k) {
            const fn = module[`tdzF${k}`];
            const closure = fn(k);
            closure(1);
            functions.push(fn);
            closures.push(closure);
        }
        const wrapped = module.tdzWrapper();
        for (let k = 0; k < functionCount; ++k) {
            const closure = wrapped[k](k);
            closure(1);
            functions.push(wrapped[k]);
            closures.push(closure);
        }
        bodies.push({ functions, closures });
    }
    const after = t.statistics();
    if (t.imports) {
        const imported = 2 * 4 * functionCount;
        t.expect("the first calls of both modules' functions and closures", t.delta(before, after), {
            imports: imported,
            tdzEnvironmentDigests: ["<=", 16],
        });
        t.expect("the environments the holder digests reach", t.delta(before, after), { tdzEnvironmentDigests: [">=", 2] });
    }

    let total = 0;
    for (const { functions } of bodies) {
        for (const fn of functions) {
            for (let round = 0; round < rounds; ++round)
                total = (total + fn(round)(round)) | 0;
        }
    }
    print(`${modules.map(module => module.tdzBindingCount).join(" and ")} bindings: ${total}`);

    if (t.twins && t.imports) {
        const size = fn => {
            const core = t.readSection(t.bodyKey(fn), "ucb.core");
            t.check(core, `no ucb.core for ${fn.name || "a closure"}`);
            return core.length;
        };
        const [large, small] = bodies;
        for (let i = 0; i < large.functions.length; ++i) {
            for (const kind of ["functions", "closures"]) {
                const largeSize = size(large[kind][i]);
                const smallSize = size(small[kind][i]);
                t.check(largeSize === smallSize, `${kind} ${i}: ucb.core holds ${largeSize} bytes under 5,000 bindings and ${smallSize} under ten`);
            }
        }
    }
    t.finish();
})(...globalThis.arguments);

// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitCacheVouchedHolderDigests=false

// What a body costs does not grow with the names its enclosing scopes declare, since the VM interns each TDZ
// environment by content and a holder digest covers an environment once. The test writes two modules into the
// sequence's scratch directory and imports them: one with 5,000 top-level bindings, 4,000 consts and 1,000 imports,
// and one with ten. Each starts with the same 200 small exported function declarations, each holding a nested closure,
// followed by a function that wraps the same bindings as consts around the same 200 declarations, as Bun wraps a
// CommonJS module, so that the module's TDZ link and the wrapper's name every binding. Both start at the same offsets
// in both modules, so their bodies differ only in their enclosing scopes.
// - In the Consumer every function and closure imports (statistics, imports). Each is a child that an imported parent's
//   decode made, so by default its holder check compares the digest the parent's body lists and computes none.
// - Sequence 1's Consumer runs with --jitCacheVouchedHolderDigests=false and computes every child's holder digest.
//   tdzEnvironmentDigests then grows by the few distinct environments those digests reach, at least one per module and
//   never one per function. In sequence 0 it stays within the same upper bound.
// - In twins runs each small function's and closure's ucb.core, read with jitcacheReadSection, has the size it has in
//   the module of ten bindings; at every import the twin check finds the core encoding and the child UFEs' fields
//   equal to the twin's.
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
        // Each module's tdzWrapper runs once in every role, so under the default delta capture its body is captured
        // without an image and imports too.
        const imported = 2 * 4 * functionCount + 2;
        t.expect("the first calls of both modules' functions and closures", t.delta(before, after), {
            imports: imported,
            tdzEnvironmentDigests: ["<=", 16],
        });
        if (t.sequence === 1)
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

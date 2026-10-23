// What invalid-material.js runs its cases with: the bodies under test, the Producer's captures and rewrite, the
// Consumer's check, and the helpers its case table is built with. Loaded after resources/ucb.js and
// resources/sections.js with load("./resources/invalid-material-cases.js", "caller relative"); it defines one global,
// invalidMaterialCases, the same in every role.
//
// The script calls invalidMaterialCases.run(t, sequenceCount, makeCases) from its main function. makeCases receives the
// helpers below and returns the table, which holds one case per sequence of the script; the sequence index picks the
// run's case. The Producer runs every function until it is captured, captures again with jitcacheDelta, and then
// rewrites one section of one body through jitcacheRewriteSection, which reseals the container, so only the lane's own
// checks can find the damage. The Consumer, run 1, calls every function once, so the damaged body is imported at its
// first call, and checks that cache activity is off at the case's step (jitcacheStatus).
var invalidMaterialCases = (function () {
    function run(t, sequenceCount, makeCases)
    {
        const S = ucbSections;

        function imPlain(x, list)
        {
            function imInner(y)
            {
                return y * 2;
            }
            let n = x;
            n++;
            const label = "invalid-material-long-string-constant";
            return ((imInner(n) + list[x & 3] + 1000) | 0) + label.length;
        }
        function imPrivate(x)
        {
            class ImBox {
                #twice() { return x * 2; }
                run() { return this.#twice(); }
            }
            return new ImBox().run();
        }
        const list = [4, 5, 6, 7];
        const functions = { imPlain, imPrivate };

        // What every role prints. In the Consumer each first call imports, so the damaged body meets its check here.
        function results()
        {
            return [imPlain(1, list), imPrivate(4)].join(" ");
        }

        // The sections of a function's body, in the Producer.
        function body(target)
        {
            const key = t.bodyKey(functions[target]);
            const read = name => {
                const bytes = t.readSection(key, name);
                t.check(bytes, `${target} has no ${name}`);
                return bytes;
            };
            return {
                key,
                identity: () => S.identity(read("ucb.identity")),
                core: () => S.functionCore(read("ucb.core")),
            };
        }
        function rewrite(key, name, offset, bytes)
        {
            return { key, name, offset, bytes: Array.from(bytes) };
        }

        // A case that rewrites `section`, ucb.identity or ucb.core, of the body of `target`, a function's name. `edit`
        // receives the parsed section (sections.js) and the body, and returns the offset and the bytes to write there.
        function sectionCase(name, step, section, target, edit)
        {
            return { name, step, edit: () => {
                const b = body(target);
                const parsed = section === "ucb.identity" ? b.identity() : b.core();
                const [offset, bytes] = edit(parsed, b);
                return rewrite(b.key, section, offset, bytes);
            } };
        }

        const cases = makeCases({
            t, S,
            identityCase: (name, edit, target = "imPlain") => sectionCase(name, "ucb.identity", "ucb.identity", target, edit),
            coreCase: (name, step, target, edit) => sectionCase(name, step, "ucb.core", target, edit),
        });
        t.check(cases.length === sequenceCount, `the table holds ${cases.length} cases for ${sequenceCount} sequences`);
        const kase = cases[t.sequence];

        if (t.role === "Producer") {
            for (let i = 0; i < 300; ++i) {
                imPlain(i, list);
                imPrivate(i);
            }
            if (t.twins) {
                jitcacheDelta();
                const { key, name, offset, bytes } = kase.edit();
                t.rewriteSection(key, name, offset, bytes);
            }
        }

        print(results());
        if (t.twins && t.imports)
            t.check(t.status() === kase.step, () => `${kase.name}: cache activity is off at ${t.status()}, expected ${kase.step}`);
        t.finish();
    }

    return { run };
})();

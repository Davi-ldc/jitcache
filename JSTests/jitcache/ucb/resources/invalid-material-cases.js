// What invalid-material.js and invalid-material-feedback.js share (SPEC-ucb.md section 13.3, invalid-material.js): the
// bodies under test, the Producer's captures and rewrite, the Consumer's check, and the helpers each script's case table
// is built with. Loaded after resources/ucb.js and resources/sections.js with
// load("./resources/invalid-material-cases.js", "caller relative"); it defines one global, invalidMaterialCases, the same
// in every role.
//
// A script calls invalidMaterialCases.run(t, sequenceCount, makeCases) from its main function. makeCases receives the
// helpers below and returns the table, which holds one case per sequence of the script; the sequence index picks the
// run's case. The Producer runs every function until it is captured, captures again with jitcacheDelta, and then
// rewrites one section of one body through jitcacheRewriteSection, which reseals the container
// (SPEC-integrator.harness.md section 5.2), so only the lane's own checks can find the damage. The Consumer, run 1,
// calls every function once, so the damaged body is imported at its first call, and checks that cache activity is off
// at the case's step (jitcacheStatus).
var invalidMaterialCases = (function () {
    function float32Bytes(value)
    {
        const view = new DataView(new ArrayBuffer(4));
        view.setFloat32(0, value, true);
        return [view.getUint8(0), view.getUint8(1), view.getUint8(2), view.getUint8(3)];
    }

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
        function imExit(x)
        {
            return x + 1;
        }
        // Its optimized code exits with two kinds, BadType on a string and Overflow at its add, so its UCB records two
        // exit sites.
        function imExits(x)
        {
            return (x + 1) | 0;
        }
        function imLinkTime(f, x)
        {
            return f.call(null, x);
        }
        function imRegExp(s)
        {
            return /\-/.test(s);
        }
        function imPrivate(x)
        {
            class ImBox {
                #twice() { return x * 2; }
                run() { return this.#twice(); }
            }
            return new ImBox().run();
        }
        function imSmall(x)
        {
            return x ^ 5;
        }
        const twice = x => x * 2;
        const list = [4, 5, 6, 7];
        const programText = "{ let imProgramTotal = 0; for (let i = 0; i < 3000; ++i) imProgramTotal = (imProgramTotal + i * 5) | 0; imProgramTotal; }";
        const functions = { imSmall, imPlain, imExit, imExits, imLinkTime, imRegExp, imPrivate };

        // What every role prints. In the Consumer each first call imports, so the damaged body meets its check here.
        function results()
        {
            return [
                imPlain(1, list), imExit(2), imExit("s"), imExits(3), imLinkTime(twice, 3), imRegExp("a-b"), imRegExp("ab"),
                imPrivate(4), imSmall(6), loadString(programText),
            ].join(" ");
        }

        // The sections of a function's body, or of the program's, in the Producer.
        function body(target)
        {
            const key = target === "program" ? S.programKeyHex(programText) : t.bodyKey(functions[target]);
            const read = name => {
                const bytes = t.readSection(key, name);
                t.check(bytes, `${target} has no ${name}`);
                return bytes;
            };
            return {
                key,
                identity: () => S.identity(read("ucb.identity")),
                feedback: () => S.feedback(read("ucb.feedback")),
                core: () => S.functionCore(read("ucb.core")),
            };
        }
        function rewrite(key, name, offset, bytes)
        {
            return { key, name, offset, bytes: Array.from(bytes) };
        }
        function setBit(bytes, start, index)
        {
            const at = start + (index >> 3);
            return [at, [bytes[at] | (1 << (index & 7))]];
        }

        // The index of imPlain's Int32 constant 1000, and of its long string constant, which has a record of its own.
        function int32Constant(core, value)
        {
            for (let i = 0; i < core.constantCount; ++i) {
                if (core.kind(i) === S.constantKind.Int32 && S.u32(core.bytes, core.slot(i)) === value)
                    return i;
            }
            throw new Error(`no Int32 constant ${value}`);
        }
        function stringRecord(core, length)
        {
            for (let i = 0; i < core.constantCount; ++i) {
                const slot = core.slot(i);
                const raw = S.u32(core.bytes, slot);
                if (core.kind(i) !== S.constantKind.String || (raw & 3) || !raw)
                    continue;
                const record = slot + S.i32(core.bytes, slot);
                if ((S.u32(core.bytes, record) & 0x7ffffff) === length)
                    return { index: i, record };
            }
            throw new Error(`no string constant record of length ${length}`);
        }
        // The first function whose body has a layout a case needs, such as a byte of padding to set.
        function functionWhere(description, predicate)
        {
            for (const name of Object.keys(functions)) {
                if (predicate(body(name)))
                    return name;
            }
            throw new Error(`no function's body has ${description}`);
        }

        // A case that rewrites `section` of the body of `target`, a function's name, "program", or a function that picks
        // one when the case runs. `edit` receives the parsed section (sections.js) and the body, and returns the offset
        // and the bytes to write there.
        function sectionCase(name, step, section, target, edit)
        {
            return { name, step, edit: () => {
                const b = body(typeof target === "function" ? target() : target);
                const parsed = section === "ucb.identity" ? b.identity() : section === "ucb.feedback" ? b.feedback() : b.core();
                const [offset, bytes] = edit(parsed, b);
                return rewrite(b.key, section, offset, bytes);
            } };
        }

        const cases = makeCases({
            t, S, setBit, int32Constant, stringRecord, float32Bytes, sectionCase,
            identityCase: (name, edit, target = "imPlain") => sectionCase(name, "ucb.identity", "ucb.identity", target, edit),
            feedbackCase: (name, edit, target = "imPlain") => sectionCase(name, "ucb.feedback", "ucb.feedback", target, edit),
            coreCase: (name, step, target, edit) => sectionCase(name, step, "ucb.core", target, edit),
            // Bodies with the layouts some rules need: a constant count that is not a multiple of 8, so that the maps and
            // the constant bits have a bit at N; padding at the end of either section; and an alignment gap before the
            // exit sites, which ucb.feedback has when its arithmetic profiles take a number of halfwords that is odd.
            unevenConstants: () => functionWhere("a constant count that is not a multiple of 8", b => b.identity().constantCount % 8),
            paddedIdentity: () => functionWhere("padding after its ucb.identity maps", b => {
                const identity = b.identity();
                return identity.end < identity.bytes.length;
            }),
            paddedFeedback: () => functionWhere("padding after its ucb.feedback constant bits", b => {
                const feedback = b.feedback();
                return feedback.end < feedback.bytes.length;
            }),
            gapBeforeExits: () => functionWhere("a gap before its exit sites", b => {
                const feedback = b.feedback();
                return feedback.exits > feedback.unary + 2 * feedback.counts.unaryArithProfiles;
            }),
        });
        t.check(cases.length === sequenceCount, `the table holds ${cases.length} cases for ${sequenceCount} sequences`);
        const kase = cases[t.sequence];

        if (t.role === "Producer") {
            noInline(imExit);
            noInline(imExits);
            for (let i = 0; i < 300; ++i) {
                imPlain(i, list);
                imLinkTime(twice, i);
                imRegExp(i & 1 ? "a-b" : "ab");
                imPrivate(i);
                imSmall(i);
            }
            loadString(programText);
            // imExit reaches the DFG on int32 arguments and exits on strings until its optimized code is jettisoned, which
            // adds its exit site to the UCB; its baseline CodeBlock is then its replacement again, so the delta captures it.
            for (let i = 0; i < 100; ++i)
                imExit(i);
            optimizeNextInvocation(imExit);
            imExit(1);
            for (let i = 0; i < 20000 && reoptimizationRetryCount(imExit) < 1; ++i)
                imExit(`s${i & 7}`);
            // imExits likewise, exiting in turn on a string and on an int32 overflow, so both sites have exits when its
            // optimized code is jettisoned.
            for (let i = 0; i < 100; ++i)
                imExits(i);
            optimizeNextInvocation(imExits);
            imExits(1);
            for (let i = 0; i < 20000 && reoptimizationRetryCount(imExits) < 1; ++i)
                imExits(i & 1 ? `s${i & 7}` : 0x7fffffff);
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

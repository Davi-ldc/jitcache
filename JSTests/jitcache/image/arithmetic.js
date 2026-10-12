// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// SPEC-image.md T1 and T2: arithmetic. Each MathIC of add, sub, mul and negate goes through each regeneration path of
// JITMathIC::generateOutOfLine (section 6.2). A site that never ran in the LLInt compiles with an empty profile to a
// patchable jump. When its first operands are int32s, that first execution builds a fast-path snippet and the first
// double then the full snippet. When they are non-numbers on both sides (strings, objects, BigInts), the fast-path attempt
// finds nothing to generate, so the same regeneration repoints the slow call and builds the full snippet with a second
// assembler, two attaches in one entry of the twins section's regeneration log (section 11.2). An add or a
// multiplication of a constant string refuses both attempts (N17), so its regeneration repoints the slow call and builds
// no snippet. A site compiled from int32 profiles builds the full snippet at its first double. The snippets jump back
// into the image (ImageOffset) and the rewritten inline start jumps to them (SnippetEntry). A body that never optimizes
// (noDFG, CannotCompile) compiles a full inline snippet with profile writes and regenerates at its first string. A body
// that can optimize, and whose operands mixed int32s and doubles before its compile, gets a full inline snippet at each
// of its four MathICs, whose slow path then calls the profiled operation with the arithmetic profile (census F4) instead
// of the repatching one with the MathIC, and never regenerates; strings then take each of those calls, negate's to the
// operationArithNegateProfiled that section 6.4 keeps for profiles. The sites without inline code (N17, census F2) are a
// + that only saw strings, "x" + y, a * that only saw BigInts and - on a BigInt. div, to_number and to_numeric write
// their profiles in place (census A14, C26, F5), and the bitwise and increment operators, whose baseline code writes
// none, reach their slow paths through slow-path thunks (A1). Every body runs 40 times, enough to compile, then at most
// six more, so it is still baseline code when the producer's delta at exit captures its snippets. Every function is
// noInline, so no optimizing compile of main reads a profile the negate fix changes (section 6.4).
// The consumer checks that its images carry (T1): the far calls' Operation pointers (B9); each MathIC's address passed
// to its slow operation (F3); the slow-path thunks of div and the bitwise and increment operators (F5, A1); the
// ValueIsTruthy thunk of each if (!run) (C4); the snippets' ImageOffset jumps back into the image (G); the SnippetEntry
// jumps of rewritten inline starts; and the arithmetic profiles each site writes or passes. A profile has one target
// whether A14 writes it, F2 passes it or F4 does, so each profile claim reads only where its site alone puts one:
// quotient's image, whose one arithmetic instruction is div, for the binary write of div's generator (A14), and
// conversions', where to_number and to_numeric write the only unary profiles baseline code references, for theirs (A14,
// C26); the images of onlyStrings, prefixed, bigProduct and bigNegation, whose one MathIC has no inline code, for F2's
// operand; and the slow calls of MathICs with inline code, between the slow path's start and the call's return point,
// for F4's.
(function main(role, scratch, artifact, sequence) {
    const lines = [];
    const warm = 40;

    function addFromEmpty(a, b, run) {
        if (!run)
            return 0;
        return a + b;
    }
    function subFromEmpty(a, b, run) {
        if (!run)
            return 0;
        return a - b;
    }
    function mulFromEmpty(a, b, run) {
        if (!run)
            return 0;
        return a * b;
    }
    function negateFromEmpty(a, run) {
        if (!run)
            return 0;
        return -a;
    }
    function addFromEmptyStrings(a, b, run) {
        if (!run)
            return "";
        return a + b;
    }
    function subFromEmptyObjects(a, b, run) {
        if (!run)
            return 0;
        return a - b;
    }
    function mulFromEmptyBigInts(a, b, run) {
        if (!run)
            return 0n;
        return a * b;
    }
    function negateFromEmptyBigInt(a, run) {
        if (!run)
            return 0n;
        return -a;
    }
    function prefixedFromEmpty(y, run) {
        if (!run)
            return "";
        return "x" + y;
    }
    function scaledFromEmpty(y, run) {
        if (!run)
            return 0;
        return "3" * y;
    }
    function addFromInt(a, b) {
        return a + b;
    }
    function subFromInt(a, b) {
        return a - b;
    }
    function mulFromInt(a, b) {
        return a * b;
    }
    function negateFromInt(a) {
        return -a;
    }
    function neverOptimized(a, b) {
        return a + b;
    }
    function fullSnippets(a, b) {
        return [a + b, a - b, a * b, -a];
    }
    function onlyStrings(a, b) {
        return a + b;
    }
    function prefixed(y) {
        return "x" + y;
    }
    function bigProduct(a, b) {
        return a * b;
    }
    function bigNegation(a) {
        return -a;
    }
    function quotient(a, b) {
        return a / b;
    }
    function conversions(x) {
        let number = +x;
        let numeric = x;
        let before = numeric++;
        return number + before + numeric;
    }
    function bitwise(a, b) {
        let c = a;
        c--;
        ++c;
        return ((a & b) | (a ^ b)) + (~a) + (a << 3) + (b >> 1) + (a >>> 2) + c;
    }
    for (const fn of [addFromEmpty, subFromEmpty, mulFromEmpty, negateFromEmpty, addFromEmptyStrings, subFromEmptyObjects,
        mulFromEmptyBigInts, negateFromEmptyBigInt, prefixedFromEmpty, scaledFromEmpty, addFromInt, subFromInt, mulFromInt,
        negateFromInt, neverOptimized, fullSnippets, onlyStrings, prefixed, bigProduct, bigNegation, quotient, conversions,
        bitwise])
        noInline(fn);
    noDFG(neverOptimized);

    // Empty profiles at compile time: int32s, then doubles.
    let empty = 0;
    for (let i = 0; i < warm; ++i)
        empty += addFromEmpty(i, 1, false) + subFromEmpty(i, 1, false) + mulFromEmpty(i, 1, false) + negateFromEmpty(i, false);
    for (let i = 0; i < 3; ++i)
        empty += addFromEmpty(i, 2, true) + subFromEmpty(i, 2, true) + mulFromEmpty(i, 2, true) + negateFromEmpty(i, true);
    for (let i = 0; i < 3; ++i)
        empty += addFromEmpty(i + 0.5, 2, true) + subFromEmpty(i + 0.25, 2, true) + mulFromEmpty(i + 0.5, 3, true) + negateFromEmpty(i + 0.75, true);
    lines.push(`from empty profiles: ${empty}`);

    // Empty profiles at compile time, non-numbers first: one regeneration attaches two assemblers. Numbers then take the
    // full snippets, and the constant strings' add and multiplication take their repointed slow calls.
    for (let i = 0; i < warm; ++i) {
        addFromEmptyStrings("a", "b", false);
        subFromEmptyObjects(i, 1, false);
        mulFromEmptyBigInts(1n, 2n, false);
        negateFromEmptyBigInt(1n, false);
        prefixedFromEmpty(i, false);
        scaledFromEmpty(i, false);
    }
    const seven = { valueOf() { return 7; } };
    const two = { valueOf() { return 2; } };
    lines.push(`from empty profiles, non-numbers first: ${addFromEmptyStrings("a", "b", true)} ${subFromEmptyObjects(seven, two, true)} ${mulFromEmptyBigInts(3n, 4n, true)} ${negateFromEmptyBigInt(5n, true)} ${prefixedFromEmpty(9, true)} ${scaledFromEmpty(9, true)}`);
    lines.push(`then numbers: ${addFromEmptyStrings(1, 2, true)} ${subFromEmptyObjects(5.5, 2, true)} ${mulFromEmptyBigInts(3, 4.5, true)} ${negateFromEmptyBigInt(6, true)} ${prefixedFromEmpty(10, true)} ${scaledFromEmpty(1.5, true)}`);

    // Int32 profiles at compile time, then doubles.
    let fromInt = 0;
    for (let i = 0; i < warm; ++i)
        fromInt += addFromInt(i, 3) + subFromInt(i, 3) + mulFromInt(i, 3) + negateFromInt(i);
    for (let i = 0; i < 3; ++i)
        fromInt += addFromInt(i + 0.5, 3) + subFromInt(3, i + 0.5) + mulFromInt(i + 0.5, 3) + negateFromInt(i + 0.5);
    lines.push(`from int32 profiles: ${fromInt}`);

    // A full inline snippet with profile writes in a CannotCompile body, regenerated by its first strings.
    let mixed = 0;
    for (let i = 0; i < warm; ++i)
        mixed += neverOptimized(i, i & 1 ? i + 0.5 : i);
    lines.push(`never optimized: ${mixed} ${neverOptimized("a", "b")} ${neverOptimized("c", 1)}`);

    // Full inline snippets in a body that can optimize, from int32s and doubles mixed before its compile; then strings,
    // which take each MathIC's slow call to its profiled operation.
    let full = 0;
    for (let i = 0; i < warm; ++i) {
        const results = fullSnippets(i & 1 ? i + 0.5 : i, i);
        full += results[0] + results[1] + results[2] + results[3];
    }
    lines.push(`full inline snippets: ${full} ${fullSnippets("4", 2).join(",")}`);

    // Sites without inline code.
    let strings = "";
    let bigs = 0n;
    for (let i = 0; i < warm; ++i) {
        strings = onlyStrings(strings.slice(-8), "s" + (i % 10));
        strings += prefixed(i % 10);
        bigs += bigProduct(BigInt(i), 3n) + bigNegation(BigInt(i));
    }
    lines.push(`without inline code: ${strings} ${bigs}`);

    let others = 0;
    for (let i = 0; i < warm + 5; ++i)
        others += quotient(i * 7, (i % 4) + 1) + conversions(i % 3 ? i : String(i)) + bitwise(i, i + 5);
    lines.push(`others: ${others}`);

    for (const line of lines)
        print(line);

    // The case happened (HARNESS.md, Tests): the producer compiled every function, so each ran its import here, and its
    // images carry the targets the header claims.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const bodies = {
            addFromEmpty, subFromEmpty, mulFromEmpty, negateFromEmpty, addFromEmptyStrings, subFromEmptyObjects,
            mulFromEmptyBigInts, negateFromEmptyBigInt, prefixedFromEmpty, scaledFromEmpty, addFromInt, subFromInt, mulFromInt,
            negateFromInt, neverOptimized, fullSnippets, onlyStrings, prefixed, bigProduct, bigNegation, quotient, conversions,
            bitwise,
        };
        sections.expectImports(role, bodies);
        sections.expectTargets(role, { call: bodies }, [
            { site: "B9", kind: "Operation", form: "Pointer" },
            { site: "F3", kind: "MathIC", form: "Pointer" },
            { site: "F4", kind: "UCBBinaryArithProfile", form: "Pointer", where: "mathICSlowCall" },
            { site: "F4", kind: "UCBUnaryArithProfile", form: "Pointer", where: "mathICSlowCall" },
            { site: "F5, A1", kind: "SlowPathThunk", form: "Call" },
            { site: "C4", kind: "BaselineThunk", a: "ValueIsTruthy", form: "Call" },
            { site: "G", kind: "ImageOffset", form: "Jump" },
            { site: "section 6.2", kind: "SnippetEntry", form: "Jump" },
        ]);
        // Each profile written in place or passed by a site without inline code, read only where that site alone puts one.
        const profileClaims = [
            [{ quotient }, "A14", "UCBBinaryArithProfile"],
            [{ conversions }, "A14, C26", "UCBUnaryArithProfile"],
            [{ onlyStrings, prefixed, bigProduct }, "F2", "UCBBinaryArithProfile"],
            [{ bigNegation }, "F2", "UCBUnaryArithProfile"],
        ];
        for (const [group, site, kind] of profileClaims)
            sections.expectTargets(role, { call: group }, [{ site, kind, form: "Pointer" }]);
    }
})(...arguments);

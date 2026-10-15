// jitcache-requires: twins
// jitcache-runs: Producer --useConcurrentJIT=false

// Captures are deterministic: in one producer, the finalize capture and a later delta capture one compilation of
// scaled, and their sections are equal. One double then regenerates scaled's add, which a * 3 leaves an int32 for its
// mul, and the next delta's image.baseline differs from the earlier one only in that MathIC's entry, its new snippet,
// the bytes and fixup of its rewritten inline start (a SnippetEntry jump), its slow-call fixup, which now names the
// operation that no longer regenerates, and the coverage rates. sum never optimizes (noDFG), so its add compiled from
// int32s and doubles is a full inline snippet with profile writes whose slow call stays the repatching operation; after
// its first strings regenerate it, the profile-write fixups of the inline snippet, now dead behind the rewritten start,
// are still there, unchanged. The jitcache functions run only in the producer, so the pin comparison runs the script's
// off path; every role runs the same calls and prints the same values.
(function main(role, scratch, artifact, sequence) {
    const producing = role === "Producer";
    const {
        imageFixupForm, imageTargetKind, imageMathICFlags, readBodySections, sameImageBytes, sameImageFixup, describeImageFixup,
        parseImageSection, imageFootprint,
    } = load("./resources/image-sections.js", "caller relative");
    function scaled(a, b) {
        return a * 3 + b;
    }
    function sum(a, b) {
        return a + b;
    }
    noInline(scaled);
    noInline(sum);
    noDFG(sum);

    const check = (condition, message) => {
        if (!condition)
            throw new Error(message);
    };
    const committed = (key, label) => {
        const sections = readBodySections(key);
        check(sections.image && sections.bakedFacts && sections.twins && sections.cbState, `no complete body ${label}`);
        return sections;
    };
    const recapture = (key, label, earlier) => {
        const result = jitcacheDelta();
        const sections = committed(key, label);
        const replaced = !sameImageBytes(earlier.cbState, sections.cbState) || !sameImageBytes(earlier.image, sections.image);
        check(result.committedBodies > 0 && replaced, `delta committed no new body ${label}: ${JSON.stringify(result)}`);
        return sections;
    };

    let total = 0;
    for (let i = 0; i < 40; ++i)
        total += scaled(i, 1);
    const scaledKey = producing ? jitcacheBodyKey(scaled, "call") : null;
    const atFinalize = producing ? committed(scaledKey, "at scaled's finalize capture") : null;
    for (let i = 0; i < 5; ++i)
        total += scaled(i, 2);
    if (producing) {
        const atDelta = recapture(scaledKey, "at the first delta", atFinalize);
        check(sameImageBytes(atFinalize.image, atDelta.image), "two captures of one compilation differ in image.baseline");
        check(sameImageBytes(atFinalize.bakedFacts, atDelta.bakedFacts), "two captures of one compilation differ in baked-facts.baseline");
        check(sameImageBytes(atFinalize.twins, atDelta.twins), "two captures of one compilation differ in image-twins.baseline");
        for (let i = 0; i < 3; ++i)
            total += scaled(i, 0.5);
        const afterRegeneration = recapture(scaledKey, "after scaled's regeneration", atDelta);
        checkOneRegeneration(parseImageSection(atDelta.image), parseImageSection(afterRegeneration.image), "scaled");
        check(sameImageBytes(atDelta.bakedFacts, afterRegeneration.bakedFacts), "scaled's regeneration changed its baked facts");
    } else {
        for (let i = 0; i < 3; ++i)
            total += scaled(i, 0.5);
    }
    print(total);

    let mixed = 0;
    for (let i = 0; i < 40; ++i)
        mixed += sum(i, i & 1 ? i + 0.5 : i);
    const sumKey = producing ? jitcacheBodyKey(sum, "call") : null;
    const beforeStrings = producing ? committed(sumKey, "at sum's finalize capture") : null;
    print(mixed, sum("a", "b"), sum("c", 1));
    if (producing) {
        const afterStrings = recapture(sumKey, "after sum's regeneration", beforeStrings);
        const before = parseImageSection(beforeStrings.image);
        const after = parseImageSection(afterStrings.image);
        const regenerated = checkOneRegeneration(before, after, "sum");
        const inside = fixup => fixup.kind === imageTargetKind.UCBBinaryArithProfile && fixup.site > regenerated.inlineStart && fixup.site <= regenerated.inlineEnd;
        const profileWrites = before.fixups.filter(inside);
        check(profileWrites.length, "sum's inline full snippet records no profile write");
        check(profileWrites.every(fixup => after.fixups.some(other => sameImageFixup(fixup, other))),
            "a profile-write fixup of sum's inline snippet changed across the rewrite");
    }

    // Exactly one MathIC regenerated between two captures of one compilation: returns its entry in before.
    function checkOneRegeneration(before, after, name) {
        const fields = ["codeSize", "arityEntryOffset", "callCount", "moldCount", "simpleSwitchTableCount", "stringSwitchTableCount", "codeMapCount", "constantPoolCount", "mathICCount"];
        for (const field of fields)
            check(before.header[field] === after.header[field], `${name}: the header's ${field} changed`);
        for (const region of ["calls", "molds", "simpleTables", "stringTables", "codeMap", "constantPool"])
            check(sameImageBytes(before[region], after[region]), `${name}: the ${region} changed`);

        const changed = before.mathICs.filter((mathIC, index) => !sameImageBytes(mathIC.entry, after.mathICs[index].entry));
        check(changed.length === 1, `${name}: ${changed.length} MathIC entries changed, not one`);
        const old = changed[0];
        const regenerated = after.mathICs[old.index];
        for (const field of ["kind", "bytecodeOffset", "inlineStart", "inlineEnd", "slowPathStart", "slowPathCall"])
            check(old[field] === regenerated[field], `${name}: MathIC ${old.index}'s ${field} changed`);
        check(!(old.flags & imageMathICFlags.hasSnippet) && regenerated.snippet && regenerated.snippetSize,
            `${name}: MathIC ${old.index} did not gain its first snippet`);
        check((regenerated.flags & ~imageMathICFlags.generateFastPathOnRepatch) === ((old.flags & ~imageMathICFlags.generateFastPathOnRepatch) | imageMathICFlags.hasSnippet),
            `${name}: MathIC ${old.index}'s flags changed beyond its snippet`);
        for (const mathIC of before.mathICs) {
            if (mathIC.index === old.index || !mathIC.snippet)
                continue;
            const same = after.mathICs[mathIC.index].snippet;
            check(same && sameImageBytes(mathIC.snippet.code, same.code) && mathIC.snippet.fixups.length === same.fixups.length
                && mathIC.snippet.fixups.every((fixup, index) => sameImageFixup(fixup, same.fixups[index])), `${name}: MathIC ${mathIC.index}'s snippet changed`);
        }

        // The rewrite's jump: a b at the inline start on ARM64, a jmp rel32 ending five bytes after it on x86_64.
        const entries = after.fixups.filter(fixup => fixup.kind === imageTargetKind.SnippetEntry && fixup.a === old.index);
        check(entries.length === 1 && entries[0].form === imageFixupForm.Jump, `${name}: MathIC ${old.index} has no single SnippetEntry jump`);
        const entry = entries[0];
        const arm64 = entry.site === old.inlineStart;
        check(arm64 || entry.site === old.inlineStart + 5, `${name}: the SnippetEntry jump sits at ${entry.site}, not at the inline start ${old.inlineStart}`);
        const rewritten = [old.inlineStart, old.inlineStart + (arm64 ? 4 : 5)];
        const covered = fixup => {
            const [start, end] = imageFootprint(fixup, before.code, arm64);
            return start >= rewritten[0] && end <= rewritten[1];
        };
        const slowCall = fixup => fixup.kind === imageTargetKind.Operation && fixup.site >= old.slowPathStart && fixup.site <= old.slowPathCall;
        const kept = before.fixups.filter(fixup => !covered(fixup));
        const added = after.fixups.filter(fixup => fixup !== entry);
        check(kept.length === added.length, `${name}: ${kept.length} fixups outside the rewrite before, ${added.length} beside the SnippetEntry after`);
        for (let index = 0; index < Math.min(kept.length, added.length); ++index) {
            const x = kept[index];
            const y = added[index];
            const retargeted = slowCall(x) && x.site === y.site && x.form === y.form && x.kind === y.kind;
            check(sameImageFixup(x, y) || retargeted, `${name}: fixup ${describeImageFixup(x)} became ${describeImageFixup(y)}`);
        }
        for (let offset = 0; offset < before.code.length; ++offset) {
            if (offset >= rewritten[0] && offset < rewritten[1])
                continue;
            check(before.code[offset] === after.code[offset], `${name}: code byte ${offset} changed outside the rewritten inline start`);
        }
        return old;
    }
})(...arguments);

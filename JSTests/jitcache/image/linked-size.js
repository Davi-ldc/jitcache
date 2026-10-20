// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// Images and snippets at their linked size. An image or a snippet is the linked size its LinkBuffer wrote, and the
// handle around it can be larger: on ARM64 whenever compaction leaves the linked size off the allocator's granule, on
// x86_64 under the libpas JIT heap whenever the size class exceeds it. The slack keeps whatever the pool held, so
// capture writes only the linked bytes and the twin check compares only those. These Function-constructor bodies grow
// one statement at a time, so their linked sizes fall at every offset from a granule boundary, and each regenerates its
// MathICs, whose snippets' sizes depend on their kinds and operands; the consumer checks that the images carry the
// snippets' jumps back into the image and the rewritten inline starts' jumps to them. A script cannot
// see a handle or what lies past a linked size, so ImageInstallTests.cpp's imageSlackStaysOutsideCaptureAndTwinCheck
// finds bodies and snippets whose handles exceed their linked sizes and makes the producer's, the restored image's and
// the twin's slack differ.
(function main(role, scratch, artifact, sequence) {
    const operators = ["+", "-", "*"];
    const bodies = [];
    for (let size = 1; size <= 24; ++size) {
        const statements = [];
        for (let i = 0; i < size; ++i)
            statements.push(`total = total ${operators[i % 3]} object.v${i % 4} + ${i};`);
        bodies.push(new Function("object", "scale", `let total = scale; ${statements.join(" ")} return total;`));
    }
    let total = 0;
    for (let i = 0; i < 40; ++i) {
        for (const body of bodies)
            total += body({ v0: i, v1: 1, v2: 2, v3: 3 }, 1);
    }
    for (let i = 0; i < 2; ++i) {
        for (const body of bodies)
            total += body({ v0: i + 0.5, v1: 1, v2: 2.25, v3: 3 }, 0.5);
    }
    print(total);

    // The case happened: the producer compiled every body, each called 42 times, so each ran its import here and met the
    // twin check's byte comparison.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const imported = { };
        bodies.forEach((body, index) => imported[`the body of ${index + 1} statements`] = body);
        sections.expectImports(role, imported);
        sections.expectTargets(role, { call: imported }, [
            { site: "a MathIC snippet's jump back into the image", kind: "ImageOffset", form: "Jump" },
            { site: "a MathIC's inline start rewritten to jump to its snippet", kind: "SnippetEntry", form: "Jump" },
        ]);
    }
})(...arguments);

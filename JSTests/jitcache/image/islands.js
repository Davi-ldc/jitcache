// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --useRandomizingExecutableIslandAllocation=true; Consumer --useConcurrentJIT=false --useRandomizingExecutableIslandAllocation=true

// Jump islands between processes, where the allocator allows it. useRandomizingExecutableIslandAllocation picks a
// random region of the pool for each allocation only where the pool is FixedVMPoolExecutableAllocator's region
// allocator, as in the ARM64 builds that link mimalloc, the release profile. There, images, snippets and the thunks
// generated on first use land more than a bl or b can reach apart. Recording keeps each external conditional branch
// behind a veneer, so only a b or bl carries a fixup, and the consumer patches through the islands the assembler's own
// writers place. The producer's decode of every fixup at capture and the consumer's read-back after patching follow
// those islands, and a failed strict check is a fault the runner reports. Twenty-four bodies, each calling thunks and
// operations from code that branches to the HandleException thunk, give both checks islands to follow. Under the
// libpas JIT heap of the ASan builds, debug and twins, the option only allocates and frees a few blocks of the
// requested size before each allocation, and on x86_64 a call reaches the whole pool, so there the script is an
// ordinary round trip.
// ImageInstallTests.cpp's imageImportPatchesThroughIslandsToFarSupport forces the distance in the twins build and
// checks that every support branch goes through an island while both of those checks and the twin check pass. The
// consumer checks that the images carry the operations' pointers and the jumps to HandleException.
(function main(role, scratch, artifact, sequence) {
    const bodies = [];
    for (let i = 0; i < 24; ++i) {
        bodies.push(new Function("object", "n", `
            let total = object.base + ${i};
            for (const value of object.values)
                total += value * n;
            try {
                if (n % ${i + 2} === 0)
                    throw new Error("body ${i}");
            } catch (error) {
                total += error.message.length;
            }
            return total + (typeof object.missing === "undefined" ? 1 : 0);
        `));
    }
    const object = { base: 3, values: [1, 2, 3] };
    let total = 0;
    for (let i = 0; i < 40; ++i) {
        for (const body of bodies)
            total += body(object, i);
    }
    print(total);

    // The case happened: the producer compiled every body, so each ran its import here.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const imported = { };
        bodies.forEach((body, index) => imported[`body ${index}`] = body);
        sections.expectImports(role, imported);
        sections.expectTargets(role, { call: imported }, [
            { site: "a far call to an operation", kind: "Operation", form: "Pointer" },
            { site: "an exception check's jump to the handler", kind: "CommonThunk", a: "HandleException", form: "Jump" },
        ]);
    }
})(...arguments);

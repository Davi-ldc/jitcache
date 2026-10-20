// jitcache-requires: twins
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false --jitcache-test-image-hook=relocation-pairs
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false --jitcache-test-image-hook=operation-pair
// jitcache-expect-twin: 0:1 coincidence executable-pool
// jitcache-expect-twin: 0:1 coincidence structure-reservation
// jitcache-expect-twin: 0:1 coincidence heap

// Relocation pairs: a fixup whose target resolves to the same address in the capturing and the importing process cannot
// show a missed patch, so the twin check reports it as a coincidence. The image test hook makes the twin check of each
// import take, as the producer value of the first fixup it compares in each relocation domain, that target's resolution
// in the restored context. In the first sequence, RelocationPairs makes the clause report exactly those fixups, one in
// each domain probe's fixups reach: the executable pool (its thunks), the structure reservation (the structure decode of
// typeof) and the heap (the VM's fields), so a domain that did not move cannot pass unnoticed. The engine image, where
// every Operation target lies, sits at its link-time address and is skipped. In the second, OperationPair forces the
// first Operation fixup, which the clause skips, so it reports nothing, and the runner fails any report it finds. probe
// is the one body these runs compile and import; the consumer checks that probe ran its import, with no LLInt
// instruction and no baseline compile, that its image carries a target in each domain named above: op_ret's jump to
// ReturnFromBaseline, the structure ID base, the soft stack limit and the far calls' operations; and then reads its own
// twin report, run1.twins, which probe's install wrote. Its jitcache functions run only in that role, so the pin
// comparison runs the script's off path.
(function main(role, scratch, artifact, sequence) {
    function probe(object) {
        if (typeof object.missing === "undefined")
            return object.value + 1;
        return 0;
    }
    let total = 0;
    for (let i = 0; i < 40; ++i)
        total += probe({ value: i });
    print(total);

    if (role !== "Consumer")
        return;
    const sections = load("./resources/image-sections.js", "caller relative");
    sections.expectImported(probe);
    sections.expectTargets(role, { call: { probe } }, [
        { site: "ret's jump to the return thunk", kind: "CommonThunk", a: "ReturnFromBaseline", form: "Jump" },
        { site: "emitLoadStructure's structure ID decode", kind: "StructureIDBase", form: "Pointer" },
        { site: "the prologue's stack limit check", kind: "VMAddress", a: "SoftStackLimit", form: "Pointer" },
        { site: "a far call to an operation", kind: "Operation", form: "Pointer" },
    ]);
    const key = jitcacheBodyKey(probe, "call");
    if (!key)
        throw new Error("probe has no body key");
    const coincidences = readFile(`${scratch}/run1.twins`).split("\n").filter(line => line.trim()).map(line => JSON.parse(line))
        .filter(entry => entry.kind === "coincidence");
    const forced = sequence === "0" ? ["executable-pool", "heap", "structure-reservation"] : [];
    const reported = coincidences.filter(entry => entry.detail.includes(`body ${key} `)).map(entry => entry.domain).sort();
    if (coincidences.length !== reported.length || reported.join(",") !== forced.join(","))
        throw new Error(`the twin report holds coincidences ${JSON.stringify(coincidences)}, where probe's import forced one in each of [${forced}]`);
})(...arguments);

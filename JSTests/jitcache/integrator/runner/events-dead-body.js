// jitcache-runs: Off --jitcache-body-events=dead-body-events.jsonl --useUnlinkedCodeBlockJettisoning=true
// jitcache-check: 0 resources/dead-body.ts

// Harness sub-SPEC H6: with --useUnlinkedCodeBlockJettisoning=true, a function that ran and was then made unreachable
// keeps its counts in the body-event dump's total after more fullGC() calls than UnlinkedCodeBlock::maxAge (7), which
// age its UnlinkedCodeBlock until its unlinked executable stops marking it. The run leaves sweepSynchronously off, so
// the UnlinkedCodeBlock's destruction can wait for the dump's own sweep. No oracle compares an Off-only sequence, so
// the script prints its counts.
//
// h6Dying comes from the Function constructor, so no CodeBlock holds its executable: the function alone keeps it, and
// the global object's cache of the constructor's last executable holds it weakly. The CodeCache keeps its unlinked
// executable. Once the function is unreachable, its executable and CodeBlock die at the next collection, and the
// unlinked executable marks the UnlinkedCodeBlock only until the collections have aged it to maxAge. A function made
// afterwards from the same text gets that unlinked executable back from the CodeCache, and the unlinked executable
// generates a new UnlinkedCodeBlock, counting from zero, only if the old one died. The checker requires that function
// to show fewer LLInt instructions than h6Dying's two calls left, and h6Dying's counts to be in the dump's total beyond
// the counts the script prints for the bodies still alive.
(function main() {
    const text = "let total = 0; for (let i = 0; i < n; ++i) total += i; return total;";
    function runBody(calls) {
        const h6Dying = new Function("n", text);
        for (let i = 0; i < calls; ++i)
            h6Dying(100);
        return jitcacheBodyEvents(h6Dying, "call");
    }
    // The collections scan the VM thread's stack conservatively, from the native frames fullGC runs in, about 1.5 KiB
    // below main's frame in a debug build, up to the stack's origin. Running runBody 64 frames, about 7 KiB, below main
    // leaves everything h6Dying's creation and calls wrote to the stack outside that range. Called from main directly,
    // h6Dying's UnlinkedCodeBlock survived the collections in a debug build, kept by a stale reference the scan found.
    function belowTheCollections(depth) {
        return depth ? belowTheCollections(depth - 1) : runBody(2);
    }
    const dead = belowTheCollections(64);
    for (let i = 0; i < 10; ++i)
        fullGC();
    const reborn = runBody(1);
    const alive = {
        main: jitcacheBodyEvents(main, "call"),
        belowTheCollections: jitcacheBodyEvents(belowTheCollections, "call"),
        runBody: jitcacheBodyEvents(runBody, "call"),
    };
    print(JSON.stringify({ dead, reborn, alive }));
})();

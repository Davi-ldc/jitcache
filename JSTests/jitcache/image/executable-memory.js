// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=1
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=2
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=2; Consumer --useConcurrentJIT=false
// jitcache-require-fault: 0:1 exec-alloc.jitcache-image
// jitcache-require-fault: 1:1 exec-alloc.jitcache-image
// jitcache-require-fault: 2:0 exec-alloc.mathic-snippet
// jitcache-expect-no-install: 0:1
// jitcache-expect-no-install: 1:1

// SPEC-image.md T8. useExecutableAllocationFuzz fails the n-th executable allocation that may fail
// (ExecutableAllocator::allocate counts every JITCompilationCanFail request). hot is the one body these runs compile:
// main's loops stay in the LLInt, and hot touches no property, so no IC allocates before it. In the first two sequences
// the producer compiles hot from int32s, regenerates its add at the first double and commits the image with its snippet
// at its delta at exit. Each consumer's first such allocation is hot's import: the image (n = 1) and then its snippet
// (n = 2), and the failure of either is ExecutableMemoryExhausted at exec-alloc.jitcache-image, which turns cache
// activity off at that import, so the consumer installs nothing. In the third, the producer's first allocation is hot's
// baseline plan and its second the snippet of hot's regeneration, whose failure raises exec-alloc.mathic-snippet before
// the regeneration falls back natively; production ends there, and the consumer installs the body the finalize capture
// committed before it, which it checks. Each failure is required to happen, and every output equals the JITCache-off
// run's.
(function main(role, scratch, artifact, sequence) {
    function hot(a, b) {
        return a + b;
    }
    let total = 0;
    for (let i = 0; i < 40; ++i)
        total += hot(i, 1);
    for (let i = 0; i < 3; ++i)
        total += hot(i + 0.5, 1);
    print(total);

    if (role === "Consumer" && sequence === "2")
        load("./resources/image-sections.js", "caller relative").expectImports(role, { hot });
})(...arguments);

// jitcache-requires: twins
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-test-image-hook=change-recorded-target; Consumer --useConcurrentJIT=false
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false --jitcache-test-image-hook=skip-patch
// jitcache-expect-fault: 0:0 image.code-fixup-targets
// jitcache-expect-fault: 1:1 image.patch-read-back
// jitcache-expect-no-install: 1

// The image lane's strict checks, each failed on purpose by a test hook. Each image test hook acts in the run whose
// options set it. In the first sequence, ChangeRecordedTarget changes the target of the first fixup of the record the
// producer's first capture reads before the capture decodes every fixup, so that decode finds the producer's code
// holding another target, a recording fault at image.code-fixup-targets that ends production before anything is
// committed; the consumer has nothing to install. In the second, SkipPatch makes the consumer's prepareImage skip its
// first patch, so the read-back after patching finds that footprint still canonical, invalid material at
// image.patch-read-back that turns cache activity off before the install, and the body runs natively. The run each hook
// acts on also checks that its fault is the first one status names; every output equals the JITCache-off run.
(function main(role, scratch, artifact, sequence) {
    function hot(object) {
        return object.value * 2 + object.offset;
    }
    let total = 0;
    for (let i = 0; i < 40; ++i)
        total += hot({ value: i, offset: 1 });
    print(total);

    const expected = sequence === "0" && role === "Producer" ? "image.code-fixup-targets"
        : sequence === "1" && role === "Consumer" ? "image.patch-read-back"
        : null;
    if (expected && jitcacheStatus() !== expected)
        throw new Error(`status names ${jitcacheStatus()}, not ${expected}`);
})(...arguments);

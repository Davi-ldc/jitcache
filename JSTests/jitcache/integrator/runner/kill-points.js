// jitcache-runs: Producer --jitcache-test-kill=before-append@1
// jitcache-runs: Producer --jitcache-test-kill=mid-stream@1
// jitcache-runs: Producer --jitcache-test-kill=after-stream@1
// jitcache-runs: Producer --jitcache-test-kill=after-envelope@1
// jitcache-runs: Producer --jitcache-test-kill=after-reread@1
// jitcache-runs: Producer --jitcache-test-kill=after-index@1
// jitcache-runs: Producer --jitcache-test-kill=after-root@1
// jitcache-runs: Producer --jitcache-delta-at-exit --jitcache-test-kill=compaction-before-rename@1
// jitcache-runs: Producer --jitcache-delta-at-exit --jitcache-test-kill=compaction-after-rename@1
// jitcache-expect-exit: 0 kill
// jitcache-check: 0:0 resources/uncommitted.ts
// jitcache-check: 1:0 resources/uncommitted.ts
// jitcache-check: 2:0 resources/uncommitted.ts
// jitcache-check: 3:0 resources/uncommitted.ts
// jitcache-check: 4:0 resources/uncommitted.ts
// jitcache-check: 5:0 resources/uncommitted.ts
// jitcache-check: 6:0 resources/committed.ts
// jitcache-check: 7:0 resources/compaction-before-rename.ts
// jitcache-check: 8:0 resources/compaction-after-rename.ts

// One sequence per kill point, each a Producer with two bodies to commit whose first commit raises SIGKILL at the point,
// which jitcache-expect-exit's kill accepts. Every point up to after-index lies before the commit's root, so the pack
// lists no body and the bytes the commit appended lie past its valid end; after-root lies after it, so the pack lists the
// body. The last two sequences kill the first compaction, which the delta at exit runs on any dead byte when a
// compaction's kill point is armed: before its rename the old pack is retired, still lists every body, and the new pack
// waits as a temporary; after it the new pack lists every body. The script prints nothing: its oracle's Off run, which
// has no kill point, runs to its end, while the killed run's output stops wherever the kill struck.
(function main() {
    function h8First(x) {
        return x + 1;
    }
    function h8Second(x) {
        return x * 2;
    }
    let total = 0;
    for (let i = 0; i < 1000; ++i)
        total += h8First(i) + h8Second(i);
})();

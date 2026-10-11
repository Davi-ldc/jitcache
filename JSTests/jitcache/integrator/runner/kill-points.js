// jitcache-runs: Producer --jitcache-test-kill=before-create@1
// jitcache-runs: Producer --jitcache-test-kill=after-create@1
// jitcache-runs: Producer --jitcache-test-kill=mid-stream@1
// jitcache-runs: Producer --jitcache-test-kill=after-stream@1
// jitcache-runs: Producer --jitcache-test-kill=after-envelope@1
// jitcache-runs: Producer --jitcache-test-kill=after-reread@1
// jitcache-runs: Producer --jitcache-test-kill=after-rename@1
// jitcache-expect-exit: 0 kill
// jitcache-check: 0:0 resources/no-temporary.ts
// jitcache-check: 1:0 resources/one-temporary.ts
// jitcache-check: 2:0 resources/one-temporary.ts
// jitcache-check: 3:0 resources/one-temporary.ts
// jitcache-check: 4:0 resources/one-temporary.ts
// jitcache-check: 5:0 resources/one-temporary.ts
// jitcache-check: 6:0 resources/no-temporary.ts

// Harness sub-SPEC H8: one sequence per kill point of section 12, each a Producer with two bodies to commit whose
// first commit raises SIGKILL at the point, which jitcache-expect-exit's kill accepts. The writer leaves one temporary
// in cache/ for the five points from after-create to after-reread, and none before it creates one or once it has
// renamed it, which the checkers count. The script prints nothing: its oracle's Off run, which has no kill point, runs
// to its end, while the killed run's output stops wherever the kill struck.
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

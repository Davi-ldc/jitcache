// jitcache-runs: Off
// jitcache-runs: Off
// jitcache-expect-exit: 1:0 3

// Harness sub-SPEC H1: each run receives its sequence index as arguments[3], and a directive written 1:0 applies to run
// 0 of the second sequence only. The second sequence's run ends on an uncaught exception, whose exit code 3 its
// directive expects; were the directive applied to the first sequence too, that sequence's clean exit would fail it,
// and were the index wrong in either sequence, the exit codes would trade places.
(function main(role, scratch, artifact, sequence) {
    if (sequence !== "0" && sequence !== "1")
        throw new Error(`arguments[3] is ${sequence}, which names no sequence of this script`);
    if (sequence === "1")
        throw new Error("the second sequence ends on an uncaught exception, as its directive expects");
})(...arguments);

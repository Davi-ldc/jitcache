// jitcache-runs: Off
// jitcache-runs: 2x Off
// jitcache-expect-exit: 1:0 3
// jitcache-expect-exit: 2:0 abort
// jitcache-fixture: passes

// Each run receives its sequence index as arguments[3], a directive written 1:0 applies to run 0 of the second
// sequence only, and a jitcache-runs line written 2x declares two sequences, the second and the third. Each sequence
// ends its own way, as its exit directive expects: the first exits cleanly, the second on an uncaught exception, whose
// exit code is 3, and the third by $vm.abort(), which every twins-mode run has. Were a directive applied to the first
// sequence too, its clean exit would fail it, and were any two sequences to receive each other's index, their endings
// would trade places. Were the counted line to give one sequence, 2:0 would name no run, and were it to give three, the
// fourth sequence would throw where no directive expects it. Every run is Off, so neither the oracle nor the pin
// comparison runs.
(function main(role, scratch, artifact, sequence) {
    if (sequence !== "0" && sequence !== "1" && sequence !== "2")
        throw new Error(`arguments[3] is ${sequence}, which names no sequence of this script`);
    if (sequence === "1")
        throw new Error("the second sequence ends on an uncaught exception, as its directive expects");
    if (sequence === "2")
        $vm.abort();
})(...arguments);

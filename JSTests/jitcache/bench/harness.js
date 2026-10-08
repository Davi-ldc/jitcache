// Validate each workload's checksum and print BENCH <name> <milliseconds>.
// Only the measured loop is timed; startup, warmup and teardown are excluded.

function reportBench(name, fn, expected, warmupIterations, measuredIterations)
{
    if (warmupIterations === undefined)
        warmupIterations = 20;
    if (measuredIterations === undefined)
        measuredIterations = 50;

    // Fixed warmup does not guarantee that every JIT tier has finished compiling.
    for (var i = 0; i < warmupIterations; ++i) {
        var result = fn();
        if (result != expected)
            throw "Error: bad result during warmup of " + name + ": " + result;
    }

    // The JSC shell's preciseTime() returns seconds; prefer it to Date.now()'s millisecond resolution.
    var nowMs = typeof preciseTime === "function"
        ? function() { return preciseTime() * 1000; }
        : Date.now;

    var before = nowMs();
    for (var i = 0; i < measuredIterations; ++i) {
        var result = fn();
        if (result != expected)
            throw "Error: bad result during measurement of " + name + ": " + result;
    }
    var after = nowMs();

    print("BENCH " + name + " " + (after - before).toFixed(3));
}

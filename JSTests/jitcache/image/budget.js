// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=4096; Consumer --useConcurrentJIT=false
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=8192
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=12288
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=16384
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=20480
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=24576
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=28672
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=32768
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=36864
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=40960
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=45056
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=49152
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=53248
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=57344
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=61440
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=65536
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=69632
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=73728
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=77824
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=81920
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=86016
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=90112
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=94208
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=98304
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=102400
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=106496
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=110592
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=114688
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=118784
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=122880
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=126976
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=131072
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=135168
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=139264
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=143360
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=147456
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=151552
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=155648
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=159744
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=163840
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false --jitcache-max-memory=262144
// jitcache-expect-fault: 0 budget.limit
// jitcache-require-fault: 0:0 budget.limit
// jitcache-require-fault: 15:0 budget.limit
// jitcache-expect-no-install: 0:1

// SPEC-image.md T12 (sections 4.5 and 4.8, I14, I19). Each sequence starts with a Producer whose limit is one more
// kRecordChargeStep (4 KiB) than the last, from 4 KiB to 160 KiB, and the last sequence's is 256 KiB, so run 0 may report
// budget.limit. guarded is the run's only baseline compilation. Each of its 48 statements loads a property, multiplies
// and adds it, stores it behind a write barrier and concatenates a constant string, a site with no inline code (N17).
// So the concatenations' operation calls and exception checks sit in the main pass, the MathICs' slow calls and theirs
// in the slow paths, and the record grows through many steps. Up to 160 KiB the steps meet every charge the producer
// makes, in order: those of guarded's compilation; those of its finalize capture and commit, which charges the writer's
// 64 KiB staging buffer and keeps it (container sub-SPEC section 8.1), so that every limit up to 64 KiB is refused by
// then; those of the MathIC regenerations that the doubles and the throwing valueOf trigger after that commit; and those
// of the delta at exit. Whatever the budget refuses, every branch the compilation and the regenerations emit stays
// linked, so each producer's output equals the JITCache-off run's. guarded throws and catches, and the last two calls
// make an operation throw in the main pass and in a slow path, so each takes its exception check's jump to
// HandleException. Which charge a given limit refuses depends on the architecture and the build, and a script cannot see
// it, so ImageRecordingTests.cpp forces each phase: imageBudgetSweepLinksEveryExceptionCheck the main pass and the slow
// paths, imageRefusalInsideEmitVeneersLinksEveryVeneer emitVeneers and imageRefusedRegenerationLinksItsSnippet a MathIC
// snippet, while imageTinyBudgetLeavesRecordsIncompleteAndReleasesEverything checks the records and the balance after the
// VM's destruction. The first sequence's limit admits only the first step of guarded's record, so its producer must
// report budget.limit and commits nothing, and its consumer has nothing to install. The 64 KiB producer must report it
// too.
(function main(role, scratch, artifact, sequence) {
    const statementCount = 48;
    const statements = [];
    for (let i = 0; i < statementCount; ++i)
        statements.push(`sum = sum + o.p${i} * ${i + 1}; t.q${i % 8} = sum; text = "k" + s;`);
    const guarded = new Function("o", "s", "t", "values", `
        let sum = 0;
        let text = "";
        ${statements.join("\n")}
        for (let i = 0; i < values.length; ++i) {
            try {
                if (values[i] < 0)
                    throw new RangeError("negative " + values[i]);
                sum = sum + values[i] * 2;
            } catch (error) {
                sum = sum - error.message.length;
            }
        }
        return sum + text.length;
    `);

    const object = {};
    for (let i = 0; i < statementCount; ++i)
        object["p" + i] = i;
    const target = {};
    let total = 0;
    for (let i = 0; i < 40; ++i)
        total += guarded(object, "s" + (i % 4), target, [i, -i - 1, i + 2]);
    print(total);

    // After guarded's compilation and its finalize capture: doubles regenerate the loop's MathICs, an operand whose valueOf
    // throws regenerates a statement's multiplication and throws from its slow call, and a string operand whose toString
    // throws throws from the first concatenation's call in the main pass.
    for (let i = 0; i < 3; ++i)
        total += guarded(object, "d", target, [i + 0.5, -1]);
    print(total);
    const valueOfThrows = Object.assign({}, object, { p5: { valueOf() { throw new Error("valueOf throws"); } } });
    try {
        print(guarded(valueOfThrows, "v", target, [1]));
    } catch (error) {
        print(error.message);
    }
    const toStringThrows = { toString() { throw new Error("toString throws"); } };
    try {
        print(guarded(object, toStringThrows, target, [1]));
    } catch (error) {
        print(error.message);
    }
})(...arguments);

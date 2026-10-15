// jitcache-requires: twins
// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; ConsumerProducer --useConcurrentJIT=false --useEagerCodeBlockJettisonTiming=true --jitcache-test-image-hook=relocation-pairs
// jitcache-expect-twin: 1 coincidence executable-pool
// jitcache-expect-twin: 1 coincidence structure-reservation
// jitcache-expect-twin: 1 coincidence heap
// jitcache-pin: off with the ConsumerProducer's options its Off run polls jitcacheBodyEvents, a function only this build has, until attached's CodeBlock ages out, and the comparison's --useGC=false lets no CodeBlock age, so a typeof guard would only trade the pin's ReferenceError for this build's failed wait

// The relocation check's ConsumerProducer case: the twin check skips every target of a body the importing process
// captured itself. The ConsumerProducer imports fromProducer, which the producer captured, and the RelocationPairs hook
// makes that import report its forced fixups in each domain they reach, which the directives declare; fromProducer's
// image carries a target in each: op_ret's jump to ReturnFromBaseline, the structure ID base, the soft stack limit and
// the far calls' operations. It compiles attached and reimported itself, so its finalize
// captures commit their bodies with its own capture-process token. Eager jettison timing then ages attached's code out,
// which empties its UCB's sharing slot while the UCB lives, and attached's next call attaches its own body to that live
// UCB and installs it. $vm.deleteAllCodeWhenIdle then cuts every UCB from its executable once the program returns, and
// reimported's next call, in a timer, imports its own body into a new UCB. Neither install may report a relocation pair,
// every other comparison still runs, and the runner fails any difference. The producer never calls attached or
// reimported, so the bodies at their keys are the ConsumerProducer's own. Every other role takes the ConsumerProducer's
// native steps, the wait for attached's code to age out included where its options age code, and the ConsumerProducer
// alone reads what JITCache did; the script prints only what fromProducer computes, the same in every role.
(function main(role, scratch, artifact, sequence) {
    function fromProducer(object) {
        if (typeof object.missing === "undefined")
            return object.value + 1;
        return 0;
    }
    function attached(object) {
        if (typeof object.missing === "undefined")
            return object.value * 2;
        return 0;
    }
    function reimported(object) {
        if (typeof object.missing === "undefined")
            return object.value * 3;
        return 0;
    }
    const check = (condition, message) => {
        if (!condition)
            throw new Error(message);
    };

    let total = 0;
    for (let i = 0; i < 40; ++i)
        total += fromProducer({ value: i });
    print(total);
    if (role === "Producer") {
        setTimeout(() => print("done"), 0);
        return;
    }

    for (let i = 0; i < 40; ++i)
        check(attached({ value: i }) === 2 * i && reimported({ value: i }) === 3 * i, `wrong results at ${i}`);

    const capturing = role === "ConsumerProducer";
    let keys = null;
    if (capturing) {
        const sections = load("./resources/image-sections.js", "caller relative");
        sections.expectTargets(role, { call: { fromProducer } }, [
            { site: "E1", kind: "CommonThunk", a: "ReturnFromBaseline", form: "Jump" },
            { site: "C27, A6", kind: "StructureIDBase", form: "Pointer" },
            { site: "B1", kind: "VMAddress", a: "SoftStackLimit", form: "Pointer" },
            { site: "B9", kind: "Operation", form: "Pointer" },
        ]);
        keys = {
            fromProducer: jitcacheBodyKey(fromProducer, "call"),
            attached: jitcacheBodyKey(attached, "call"),
            reimported: jitcacheBodyKey(reimported, "call"),
        };
        check(keys.fromProducer && keys.attached && keys.reimported, `a body has no key: ${JSON.stringify(keys)}`);
        check(jitcacheReadSection(keys.attached, "image.baseline") && jitcacheReadSection(keys.reimported, "image.baseline"),
            "the ConsumerProducer committed no body for attached or reimported");
    }

    // attached's baseline counter moved after its install, so the first aging check renews its lease and a later one
    // jettisons it, which leaves the executable without a CodeBlock and the UCB's sharing slot empty. A run whose options
    // do not age code takes the steps that follow without the wait.
    if (jscOptions().useEagerCodeBlockJettisonTiming) {
        (function ageAttachedOut() {
            for (let attempt = 0; attempt < 250; ++attempt) {
                if (jitcacheBodyEvents(attached, "call") === null)
                    return;
                sleepSeconds(0.02);
                fullGC();
            }
            throw new Error("attached's CodeBlock never aged out");
        })();
    }
    const beforeAttach = capturing ? { attaches: jitcacheUCBStatistics().attaches, installs: jitcacheProgress().installs } : null;
    check(attached({ value: 5 }) === 10, "attached's result changed");
    if (capturing) {
        check(jitcacheUCBStatistics().attaches > beforeAttach.attaches, "attached's call did not attach its body");
        check(jitcacheProgress().installs > beforeAttach.installs, "attached's call did not install its body");
    }

    $vm.deleteAllCodeWhenIdle();
    setTimeout(() => {
        fullGC();
        const beforeImport = capturing ? { imports: jitcacheUCBStatistics().imports, installs: jitcacheProgress().installs } : null;
        check(reimported({ value: 5 }) === 15, "reimported's result changed");
        if (capturing) {
            check(jitcacheUCBStatistics().imports > beforeImport.imports, "reimported's call did not import its body");
            check(jitcacheProgress().installs > beforeImport.installs, "reimported's call did not install its body");

            const coincidences = readFile(`${scratch}/run1.twins`).split("\n").filter(line => line.trim()).map(line => JSON.parse(line))
                .filter(entry => entry.kind === "coincidence");
            const naming = key => coincidences.filter(entry => entry.detail.includes(`body ${key} `));
            const forced = new Set(naming(keys.fromProducer).map(entry => entry.domain));
            check(["executable-pool", "heap", "structure-reservation"].every(domain => forced.has(domain)),
                `fromProducer's import forced coincidences in [${[...forced]}] only`);
            check(!naming(keys.attached).length && !naming(keys.reimported).length,
                `an import of a body this process captured reported ${JSON.stringify([...naming(keys.attached), ...naming(keys.reimported)])}`);
        }
        print("done");
    }, 0);
})(...arguments);

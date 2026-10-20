// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// Strict equality against string constants in imported images, each import twin-checked. stricteq and nstricteq as
// values, and jstricteq and jnstricteq as branch conditions, compare a string constant the UCB owns, on the left at
// some sites and on the right at others, by its atom's address (UCBConstantAtom); generation makes every string
// constant an atom, and so does the import of the UCB that holds it. StrictEqualNode::emitBytecode and
// BinaryOpNode::emitBytecode move a string literal on the left of === or !== to the right, so the sites that keep the
// constant on the left write it as a template literal without substitutions, which is no StringNode. Equalities against
// null and a boolean compare bitwise and never reach the atom test, and one between two variables compares generically.
// The inputs are those constants, equal strings built at run time, which are no atoms, other strings and other types.
// The consumer checks that the image of each function with a string constant compares by that constant's atom at its
// one site, and that the other images do not.
(function main(role, scratch, artifact, sequence) {
    function valueRight(x) {
        return x === "jitcache-right-constant";
    }
    function valueLeft(x) {
        return `jitcache-left-constant` === x;
    }
    function notValueRight(x) {
        return x !== "jitcache-right-constant";
    }
    function notValueLeft(x) {
        return `jitcache-left-constant` !== x;
    }
    function branchRight(x) {
        if (x === "jitcache-right-constant")
            return 1;
        return 2;
    }
    function branchLeft(x) {
        if (`jitcache-left-constant` === x)
            return 1;
        return 2;
    }
    function notBranchRight(x) {
        if (x !== "jitcache-right-constant")
            return 1;
        return 2;
    }
    function notBranchLeft(x) {
        if (`jitcache-left-constant` !== x)
            return 1;
        return 2;
    }
    function againstNull(x) {
        return x === null;
    }
    function againstTrue(x) {
        return x !== true;
    }
    function betweenVariables(x, y) {
        return x === y;
    }

    const side = ["left", "right"];
    const inputs = [
        "jitcache-left-constant",
        "jitcache-right-constant",
        "jitcache-" + side[0] + "-constant",
        "jitcache-" + side[1] + "-constant",
        "jitcache-other",
        7,
        null,
        true,
        undefined,
        { },
    ];
    const counts = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    for (let i = 0; i < 60; ++i) {
        const x = inputs[i % inputs.length];
        const y = inputs[(i * 3) % inputs.length];
        counts[0] += valueRight(x);
        counts[1] += valueLeft(x);
        counts[2] += notValueRight(x);
        counts[3] += notValueLeft(x);
        counts[4] += branchRight(x);
        counts[5] += branchLeft(x);
        counts[6] += notBranchRight(x);
        counts[7] += notBranchLeft(x);
        counts[8] += againstNull(x);
        counts[9] += againstTrue(x);
        counts[10] += betweenVariables(x, y);
    }
    print(counts.join(" "));

    // The case happened: the producer compiled every function, so each ran its import here, and each image compares by
    // atom exactly where its function has a string constant.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const byAtom = {
            valueRight, valueLeft, notValueRight, notValueLeft, branchRight, branchLeft, notBranchRight, notBranchLeft,
        };
        const generic = { againstNull, againstTrue, betweenVariables };
        sections.expectImports(role, { ...byAtom, ...generic });
        sections.expectTargets(role, { call: byAtom }, [
            { site: "the strict-equality templates' atom comparison", kind: "UCBConstantAtom", form: "Pointer" },
        ]);
        if (sections.checksImports(role)) {
            const atomFixups = (fn, label) => sections.bodyTargets(fn, "call", label).fixups
                .filter(fixup => fixup.kind === sections.imageTargetKind.UCBConstantAtom).length;
            for (const [label, fn] of Object.entries(byAtom)) {
                const count = atomFixups(fn, label);
                if (count !== 1)
                    throw new Error(`${label}'s image compares by atom at ${count} sites, not 1`);
            }
            for (const [label, fn] of Object.entries(generic)) {
                const count = atomFixups(fn, label);
                if (count)
                    throw new Error(`${label}'s image compares by atom at ${count} sites`);
            }
        }
    }
})(...arguments);

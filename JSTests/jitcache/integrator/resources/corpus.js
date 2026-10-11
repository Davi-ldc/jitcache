// The corpus finalize-capture.js and default-mode.js share (SPEC-integrator.md section 15.2): bodies of the shapes
// baseline code takes, arithmetic with its MathICs, property and call sites, a closure and the arrow it returns, a string
// switch, a constructor and a handler that catches. integratorCorpus drives each body 60 times in one loop, so each
// compiles at its 34th call, as the LLInt's warm-up threshold of 500 points at 15 points a call gives, and stays far
// below the baseline counter's threshold for the DFG; the loop's 60 iterations keep integratorCorpus itself, and with
// it every caller, in the LLInt. Every body is a local function, so nothing the corpus builds outlives the call, and the
// total does not depend on the run's role.

function integratorCorpus() {
    function corpusArithmetic(a, b) {
        return (a * 3 + b) | 0;
    }

    function corpusProperties(point) {
        return point.x - point.y;
    }

    function corpusCallee(value, step) {
        return value + step;
    }

    function corpusCaller(callee, value) {
        return callee(value, 2) + 1;
    }

    function corpusClosure(n) {
        const doubled = n * 2;
        return () => doubled + n;
    }

    function corpusStrings(word) {
        switch (word) {
        case "alpha":
            return 1;
        case "beta":
            return 2;
        case "gamma":
            return 3;
        default:
            return 4;
        }
    }

    function CorpusPoint(x, y) {
        this.x = x;
        this.y = y;
    }

    function corpusThrower(i) {
        try {
            if (i % 5 === 0)
                throw new Error(`corpus ${i}`);
            return i;
        } catch (error) {
            return error.message.length;
        }
    }

    const words = ["alpha", "beta", "gamma", "delta"];
    let arrow = null;
    let total = 0;
    for (let i = 0; i < 60; ++i) {
        total = (total + corpusArithmetic(i, 1)) | 0;
        total = (total + corpusProperties({ x: i, y: 1 })) | 0;
        total = (total + corpusCaller(corpusCallee, i)) | 0;
        arrow = corpusClosure(i);
        total = (total + arrow()) | 0;
        total = (total + corpusStrings(words[i & 3])) | 0;
        const point = new CorpusPoint(i, 2);
        total = (total + point.x + point.y) | 0;
        total = (total + corpusThrower(i)) | 0;
    }

    return {
        total,
        bodies: [
            [corpusArithmetic, "call"],
            [corpusProperties, "call"],
            [corpusCallee, "call"],
            [corpusCaller, "call"],
            [corpusClosure, "call"],
            [arrow, "call"],
            [corpusStrings, "call"],
            [CorpusPoint, "construct"],
            [corpusThrower, "call"],
        ],
    };
}

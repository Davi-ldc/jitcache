//@ requireOptions("--useLLInt=false")

// With the LLInt off, negate's CodeBlock is born in baseline before its body first runs, so the MathIC of its
// negate site starts from an empty profile. The first int32 makes the IC generate an int32 fast path, and the
// first double repoints its slow call to the non-repatching operation and gives it the full snippet. From then
// on, every operand the snippet leaves to the slow path (zero, the int32 minimum and every non-number) reaches
// that operation, which observes it through the IC's profile, and the last phase tiers negate up with what that
// profile learned. JS can check only the results, so this test also passes when the operation writes its
// observations into the IC instead; the live-VM check of this path in
// Source/JavaScriptCore/jitcache/tests/ImageRecordingTests.cpp is the one that reads the profile.

function describe(value) {
    if (Object.is(value, -0))
        return "-0";
    if (typeof value === "bigint")
        return value + "n";
    return String(value);
}

function shouldBe(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": expected " + describe(expected) + " but got " + describe(actual));
}

function shouldThrow(run, errorType, what) {
    let threw = false;
    try {
        run();
    } catch (error) {
        threw = true;
        if (!(error instanceof errorType))
            throw new Error(what + ": threw " + String(error));
    }
    if (!threw)
        throw new Error(what + ": did not throw");
}

function negate(x) {
    return -x;
}
noInline(negate);

class ValueOfError extends Error { }

const largeBigInt = 2n ** 80n;
const cases = [
    [0, -0, "zero"],
    [-0, 0, "negative zero"],
    [-2147483648, 2147483648, "int32 minimum"],
    [2147483647, -2147483647, "int32 maximum"],
    [7, -7, "int32"],
    [2.5, -2.5, "double"],
    [NaN, NaN, "NaN"],
    [Infinity, -Infinity, "infinity"],
    ["3", -3, "numeric string"],
    ["x", NaN, "non-numeric string"],
    ["", -0, "empty string"],
    [true, -1, "true"],
    [false, -0, "false"],
    [null, -0, "null"],
    [undefined, NaN, "undefined"],
    [{ valueOf() { return 4; } }, -4, "object whose valueOf returns an int32"],
    [{ valueOf() { return 4.5; } }, -4.5, "object whose valueOf returns a double"],
    [{ valueOf() { return 6n; } }, -6n, "object whose valueOf returns a BigInt"],
    [5n, -5n, "small BigInt"],
    [-5n, 5n, "negative small BigInt"],
    [largeBigInt, -largeBigInt, "large BigInt"],
    [-largeBigInt, largeBigInt, "negative large BigInt"],
];

const symbol = Symbol("negate");
const throwingObject = { valueOf() { throw new ValueOfError("valueOf"); } };

function checkThrows() {
    shouldThrow(() => negate(symbol), TypeError, "symbol");
    shouldThrow(() => negate(throwingObject), ValueOfError, "object whose valueOf throws");
}

// Regeneration from the empty profile.
shouldBe(negate(1), -1, "first int32");
shouldBe(negate(1.5), -1.5, "first double");

// Every case once in baseline, each non-number first seen after the regeneration.
for (const [operand, expected, what] of cases)
    shouldBe(negate(operand), expected, what);
checkThrows();

// Tier-up with the profile the slow path filled.
for (let i = 0; i < testLoopCount; ++i) {
    const [operand, expected, what] = cases[i % cases.length];
    shouldBe(negate(operand), expected, what);
    if (!(i % 64))
        checkThrows();
}

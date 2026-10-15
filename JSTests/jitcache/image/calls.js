// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// Calls in imported images, each import twin-checked. Every call's fast path loads LLInt::defaultCall() (ProcessThunk);
// a callee called with fewer arguments than it declares enters through its arity check, which pads the frame and calls
// the arity fixup; varargs calls and constructs spread, apply and forward arguments; strict functions tail-call,
// directly and with varargs; a site that meets many callees goes polymorphic and then virtual; a constructor reaches its
// base through super. slowThenString runs typeof, an opcode whose template only calls its slow path (DEFINE_SLOW_OP),
// and stores a string constant at once, so on ARM64, outside the debug builds' consistency checks, the slow path's bl and
// the store's movz put a Call and a Pointer fixup at one site.
// The consumer checks that its images carry: the default call target LLInt::defaultCall() as a Pointer; the arity
// fixup's Call and the soft stack limit the arity check compares; typeof's slow-path call; the string constant moved as
// a cell; op_ret's jump to ReturnFromBaseline; and op_enter's slow-path call to its handler.
(function main(role, scratch, artifact, sequence) {
    function sum3(a, b, c) {
        return (a ?? 0) + (b ?? 0) + (c ?? 0);
    }
    function arity(a, b, c, d) {
        return [a, b, c, d].filter(value => value !== undefined).length;
    }
    function viaApply(args) {
        return sum3.apply(null, args);
    }
    function viaSpread(args) {
        return sum3(...args);
    }
    function Pair(left, right) {
        this.left = left;
        this.right = right;
    }
    function construct(args) {
        const pair = new Pair(...args);
        return pair.left + pair.right;
    }
    function countdown(n, accumulator) {
        "use strict";
        if (n === 0)
            return accumulator;
        return countdown(n - 1, accumulator + 1);
    }
    function tailSpread(args) {
        "use strict";
        return sum3(...args);
    }
    function tailForward() {
        "use strict";
        return sum3.apply(null, arguments);
    }
    function callEach(callees, x) {
        let total = 0;
        for (const callee of callees)
            total += callee(x);
        return total;
    }
    class Base {
        constructor(value) {
            this.value = value;
        }
    }
    class Derived extends Base {
        constructor(value) {
            super(value * 2);
            this.extra = value;
        }
    }
    noInline(Derived);
    function slowThenString(value) {
        let kind = typeof value;
        let label = "jitcache-after-slow-op";
        return kind + ":" + label.length;
    }

    const few = [x => x + 1, x => x * 2, x => x - 3];
    const many = [];
    for (let i = 0; i < 12; ++i)
        many.push(new Function("x", `return x + ${i};`));

    let total = 0;
    let text = "";
    for (let i = 0; i < 60; ++i) {
        total += arity(i) + arity(i, i) + arity(i, i, i, i);
        total += viaApply([i, 1, 2]) + viaApply([i]) + viaSpread([i, 3]) + construct([i, 4]);
        total += countdown(i % 10, 0) + tailSpread([i, 5, 6]) + tailForward(i, 7);
        total += callEach(few, i) + callEach(many, i);
        const derived = new Derived(i);
        total += derived.value + derived.extra;
        text += slowThenString([i, "s", null, undefined, { }, () => i][i % 6]).length;
    }
    print(total);
    print(text);

    // The case happened (HARNESS.md, Tests): the producer compiled every function the loop calls, each at least 60 times,
    // so each ran its import here, the constructors at their first construction, and their images carry the targets the
    // header claims.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const bodies = { sum3, arity, viaApply, viaSpread, construct, countdown, tailSpread, tailForward, callEach, slowThenString };
        few.forEach((fn, index) => bodies[`few[${index}]`] = fn);
        many.forEach((fn, index) => bodies[`many[${index}]`] = fn);
        const constructors = { Pair, Base, Derived };
        sections.expectImports(role, bodies);
        sections.expectImports(role, constructors, "construct");
        sections.expectTargets(role, { call: bodies, construct: constructors }, [
            { site: "A15", kind: "ProcessThunk", a: "DefaultCall", form: "Pointer" },
            { site: "B2", kind: "ProcessThunk", a: "ArityFixup", form: "Call" },
            { site: "A8, B4", kind: "VMAddress", a: "SoftStackLimit", form: "Pointer" },
            { site: "A1", kind: "SlowPathThunk", form: "Call" },
            { site: "C1, B6", kind: "UCBConstantCell", form: "Pointer" },
            { site: "E1", kind: "CommonThunk", a: "ReturnFromBaseline", form: "Jump" },
            { site: "C17", kind: "BaselineThunk", a: "OpEnterHandler", form: "Call" },
        ]);
    }
})(...arguments);

// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// Exceptions and catch in imported images, each import twin-checked. The bodies throw from JavaScript, from operations,
// from a getter an IC calls and from native functions, and catch in baseline code, so their images hold op_throw's
// handler thunk, the exception checks after operations, and op_catch's references: the VM, topEntryFrame and
// targetMachinePCForThrow. A call the LLInt leaves by an exception earns only its prologue's 5 of the 15 tier-up points,
// so the loops run long enough for the producer to compile the throwing functions too: thrower returns at one call in
// seven, so the first loop runs 140 times, and deepest, middle and outer never return, so the stack loop runs 120 times.
// The consumer installs each function at its first call and checks that it did. The output, a stack trace included,
// must equal the JITCache-off run's; concurrent JIT stays off so that every import meets its twin check.
// The consumer checks that its images carry: op_throw's jump to its handler thunk; the pending exception each exception
// check tests, and debug builds compare; the jumps to HandleException; op_catch's topEntryFrame, VM and
// targetMachinePCForThrow; the ValueIsFalsey thunk of each if over a number; and, in debug builds, which every twins
// build is, the topCallFrame store before each operation call.
(function main(role, scratch, artifact, sequence) {
    let finallyCount = 0;

    function thrower(kind) {
        switch (kind) {
        case 0:
            throw new Error("an error " + kind);
        case 1:
            throw new TypeError("a type error");
        case 2:
            return null.property;
        case 3:
            return JSON.parse("{");
        case 4:
            throw { kind };
        case 5:
            return new Array(-1);
        default:
            return kind * 2;
        }
    }

    function catcher(kind) {
        try {
            return "returned " + thrower(kind);
        } catch (error) {
            return error instanceof Error ? `${error.name}: ${error.message}` : `value ${JSON.stringify(error)}`;
        } finally {
            ++finallyCount;
        }
    }

    function rethrower(n) {
        try {
            try {
                if (n % 3 === 0)
                    throw new RangeError("inner " + n);
                return "kept " + n;
            } catch (error) {
                if (n % 2)
                    throw error;
                return "caught " + error.message;
            }
        } catch (error) {
            return "rethrown " + error.message;
        }
    }

    const guarded = {
        get bad() {
            throw new Error("from a getter");
        },
        good: 1,
    };
    function readProperty(object, name) {
        try {
            return object[name];
        } catch (error) {
            return error.message;
        }
    }

    function bindingFree(n) {
        try {
            if (n & 1)
                throw n;
            return "even";
        } catch {
            return "odd";
        }
    }

    function deepest(n) {
        throw new Error("for the stack " + n);
    }
    function middle(n) {
        return deepest(n) + 1;
    }
    function outer(n) {
        return middle(n) + 1;
    }

    const results = new Map();
    const note = result => results.set(result, (results.get(result) ?? 0) + 1);
    for (let i = 0; i < 140; ++i) {
        note(catcher(i % 7));
        note(rethrower(i));
        note(readProperty(guarded, i % 2 ? "bad" : "good"));
        note(bindingFree(i));
    }
    for (const [result, count] of results)
        print(`${count} x ${result}`);
    print(`finally ran ${finallyCount} times`);

    let stack = null;
    for (let i = 0; i < 120; ++i) {
        try {
            outer(i);
        } catch (error) {
            stack = error.stack;
        }
    }
    print(stack);

    // The case happened (HARNESS.md, Tests): the getter, which throws at every call, is the one body here the producer
    // never compiles. The images of the others carry the targets the header claims.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const bodies = { thrower, catcher, rethrower, readProperty, bindingFree, deepest, middle, outer };
        sections.expectImports(role, bodies);
        sections.expectTargets(role, { call: bodies }, [
            { site: "C5", kind: "BaselineThunk", a: "OpThrowHandler", form: "Jump" },
            { site: "A3, B7", kind: "VMAddress", a: "Exception", form: "Pointer" },
            { site: "A17", kind: "CommonThunk", a: "HandleException", form: "Jump" },
            { site: "C8, A9", kind: "VMAddress", a: "TopEntryFrame", form: "Pointer" },
            { site: "C9, C10", kind: "VMAddress", a: "VM", form: "Pointer" },
            { site: "C11, A12", kind: "VMAddress", a: "TargetMachinePCForThrow", form: "Pointer" },
            { site: "C4", kind: "BaselineThunk", a: "ValueIsFalsey", form: "Call" },
            { site: "A2", kind: "VMAddress", a: "TopCallFrame", form: "Pointer" },
        ]);
    }
})(...arguments);

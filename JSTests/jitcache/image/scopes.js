// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// Scopes in imported images, each import twin-checked. Reads and writes of a global var, a global let and a global
// property, one of them promoted from unresolved, reach resolve_scope, get_from_scope and put_to_scope through the scope
// thunks, whose keys the native selection fixes. Closures read variables one to twelve scopes up, below and above the
// depth of eight at which resolve_scope loops, and their ClosureVar shapes are baked facts. with makes a dynamic scope;
// a sloppy direct eval injects a var, which turns the accesses around it into their var-injection forms; direct eval
// also runs as call_direct_eval, whose slow case falls back to a virtual call when eval is shadowed, after it gives a
// site whose call data the VM still shares data of its own through operationEnsureCallLinkInfo. new Function, an
// indirect eval and the eval code are roots and children of their own. A function a tainted source defines enters with
// the taint store of op_enter when $vm.runTaintedString exists, as in twins runs.
// The consumer checks that its images carry: the resolve_scope thunks of the global var, property and lexical var
// readGlobals reads, and of the var-injection form inner's sloppy-eval scope gives its read of globalVariable; the
// get_from_scope thunks of the same four, by the keys the native selection links; put_to_scope's slow-path thunk; the
// regular virtual call of call_direct_eval's slow case; and the taint flag op_enter stores.
var globalVariable = 1;
let globalLexical = 2;
globalThis.globalProperty = 3;

(function main(role, scratch, artifact, sequence) {
    function readGlobals() {
        return globalVariable + globalLexical + globalProperty;
    }
    function writeGlobals(value) {
        globalVariable = value;
        globalLexical = value + 1;
        globalProperty = value + 2;
    }
    function lateReader() {
        return typeof lateGlobal === "undefined" ? -1 : lateGlobal;
    }

    function nest() {
        let v0 = 0;
        return function d1() {
            let v1 = 1;
            return function d2() {
                let v2 = 2;
                return function d3() {
                    let v3 = 3;
                    return function d4() {
                        let v4 = 4;
                        return function d5() {
                            let v5 = 5;
                            return function d6() {
                                let v6 = 6;
                                return function d7() {
                                    let v7 = 7;
                                    return function d8() {
                                        let v8 = 8;
                                        return function d9() {
                                            let v9 = 9;
                                            return function d10() {
                                                let v10 = 10;
                                                return function d11() {
                                                    let v11 = 11;
                                                    return function d12(n) {
                                                        v0 += n;
                                                        v11 += 1;
                                                        return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11;
                                                    };
                                                };
                                            };
                                        };
                                    };
                                };
                            };
                        };
                    };
                };
            };
        };
    }
    const deepest = nest()()()()()()()()()()()();

    function shallow() {
        let near = 1;
        return function closer() {
            let nearer = 2;
            return function closest(n) {
                near += n;
                return near + nearer;
            };
        };
    }
    const shallowest = shallow()();

    function withRead(object) {
        with (object) {
            return x + y + globalVariable;
        }
    }

    function injected(code) {
        var local = 5;
        eval(code);
        return function inner(n) {
            local += n;
            return local + (typeof injectedVar === "number" ? injectedVar : 0) + globalVariable;
        };
    }
    const injectedReader = injected("var injectedVar = 40;");
    const plainReader = injected("");

    function directEval(x) {
        return eval("x * 2 + globalLexical");
    }
    function shadowedEval(x) {
        var eval = text => text + "!";
        return eval(x);
    }

    const constructed = new Function("a", "b", "return a * b + globalVariable;");
    const indirect = (0, eval)("(function indirectBody(n) { return n + globalProperty; })");
    const taintedSource = "(function taintedLoop(n) { let s = 0; for (let i = 0; i < n; ++i) s += i; return s; })";
    const runsTainted = typeof $vm === "object" && typeof $vm.runTaintedString === "function";
    const tainted = runsTainted ? $vm.runTaintedString(taintedSource) : (0, eval)(taintedSource);

    let total = 0;
    let text = "";
    for (let i = 0; i < 60; ++i) {
        if (i === 20)
            globalThis.lateGlobal = 9;
        total += readGlobals() + lateReader();
        writeGlobals(i);
        total += deepest(1) + shallowest(1);
        total += withRead(i % 2 ? { x: i, y: 1 } : { y: 2, x: 1 });
        total += injectedReader(1) + plainReader(2);
        total += directEval(i);
        text += shadowedEval(i % 10);
        total += constructed(i, 2) + indirect(i) + tainted(i % 7);
    }
    print(total);
    print(text);

    // The case happened: the producer compiled every function the loop calls, each 60 times, and both readers of injected
    // share one executable, so each ran its import here. A function's ClosureVar accesses are resolved when its first
    // CodeBlock links, from the same scopes in every run, so the baked facts match.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const bodies = {
            readGlobals, writeGlobals, lateReader, deepest, shallowest, withRead, injectedReader, directEval, shadowedEval,
            constructed, indirect, tainted,
        };
        sections.expectImports(role, bodies);
        sections.expectTargets(role, { call: bodies }, [
            { site: "resolve_scope's thunk call", kind: "BaselineThunk", a: "ResolveScopeGlobalVar", form: "Call" },
            { site: "resolve_scope's thunk call", kind: "BaselineThunk", a: "ResolveScopeGlobalProperty", form: "Call" },
            { site: "resolve_scope's thunk call", kind: "BaselineThunk", a: "ResolveScopeGlobalLexicalVar", form: "Call" },
            {
                site: "resolve_scope's thunk call",
                kind: "BaselineThunk", a: "ResolveScopeGlobalVarWithVarInjectionChecks", form: "Call",
            },
            { site: "get_from_scope's thunk call", kind: "BaselineThunk", a: "GetFromScopeGlobalVar", form: "Call" },
            { site: "get_from_scope's thunk call", kind: "BaselineThunk", a: "GetFromScopeGlobalProperty", form: "Call" },
            { site: "get_from_scope's thunk call", kind: "BaselineThunk", a: "GetFromScopeGlobalLexicalVar", form: "Call" },
            {
                site: "get_from_scope's thunk call",
                kind: "BaselineThunk", a: "GetFromScopeGlobalVarWithVarInjectionChecks", form: "Call",
            },
            { site: "put_to_scope's slow-path thunk call", kind: "BaselineThunk", a: "SlowOpPutToScope", form: "Call" },
            { site: "direct eval's slow-case virtual call", kind: "VirtualCallThunk", a: "Regular", form: "Call" },
            { site: "enter's tainted-code flag store", kind: "VMAddress", a: "MightBeExecutingTaintedCode", form: "Pointer" },
        ]);
    }
})(...arguments);

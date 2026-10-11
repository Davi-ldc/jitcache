// jitcache-runs: Off

// Harness sub-SPEC H5, on explicit roots: each of eight differences changes what jitcacheDescribeHeap gives of an
// object that reaches it, while a value only the shell's arguments reaches, or only the global object reaches, changes
// nothing, since an explicit-root description writes the global object without its edges. Any other outcome throws.
(function main() {
    const describe = value => jitcacheDescribeHeap(value);
    function differs(difference, make) {
        if (describe(make(1)) === describe(make(2)))
            throw new Error(`${difference}: two objects that differ in it describe alike`);
    }

    differs("one reachable value", value => ({ value }));
    differs("one property attribute", value => Object.defineProperty({ }, "property", { value: 1, writable: true, enumerable: true, configurable: value === 1 }));
    differs("the order in which two properties were added", value => {
        const object = { };
        if (value === 1) {
            object.first = 1;
            object.second = 2;
        } else {
            object.second = 2;
            object.first = 1;
        }
        return object;
    });
    differs("one prototype", value => Object.create(value === 1 ? Object.prototype : Array.prototype));
    differs("one closure variable", value => ({ read: (captured => () => captured)(value) }));
    differs("one Map entry", value => new Map([["entry", value]]));
    const key = { };
    differs("one weak-map entry whose key stays reachable", value => ({ key, map: new WeakMap([[key, value]]) }));
    class Private {
        #field;
        constructor(value) {
            this.#field = value;
        }
    }
    differs("one private field", value => new Private(value));

    // A function reaches the global object through its scope chain.
    const object = { read: () => 1 };
    const before = describe(object);
    globalThis.arguments.push({ onlyTheArgumentsReachThis: true });
    if (describe(object) !== before)
        throw new Error("a value only the shell's arguments reaches changed a description of explicit roots");
    globalThis.onlyTheGlobalObjectReachesThis = { value: 1 };
    if (describe(object) !== before)
        throw new Error("a value only the global object reaches changed a description of explicit roots");
    print("explicit roots describe what they reach");
})();

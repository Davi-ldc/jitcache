// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer

// Harness sub-SPEC H5: sequence k makes its Producer run and the oracle's Off run, otherwise equal, differ in one thing
// JavaScript can reach at the end of the run, and each such difference must change the end-of-run description. The
// runner's self-test therefore expects every sequence to fail on its heap description and on nothing else, while
// describe-alike.js, which runs the same two runs with differences JavaScript cannot see, is the control that passes.
// The module binding is describe-module-binding.mjs's, and the eight differences that also change a description of
// explicit roots are describe-explicit-roots.js's.
let h5GlobalLet = arguments[3] === "8" && arguments[0] === "Producer" ? "the Producer's value" : "every other run's value";
(function main(role, scratch, artifact, sequence) {
    const variant = role === "Producer";
    switch (sequence) {
    case "0":
        // One reachable value.
        globalThis.h5Value = variant ? 2 : 1;
        break;
    case "1": {
        // One property attribute.
        const object = {};
        Object.defineProperty(object, "property", { value: 1, writable: true, enumerable: true, configurable: !variant });
        globalThis.h5Attributes = object;
        break;
    }
    case "2": {
        // The order in which two properties were added.
        const object = {};
        if (variant) {
            object.second = 2;
            object.first = 1;
        } else {
            object.first = 1;
            object.second = 2;
        }
        globalThis.h5Order = object;
        break;
    }
    case "3":
        // One prototype.
        globalThis.h5Prototype = Object.create(variant ? Array.prototype : Object.prototype);
        break;
    case "4":
        // One closure variable.
        globalThis.h5Closure = (captured => () => captured)(variant ? 2 : 1);
        break;
    case "5":
        // One Map entry.
        globalThis.h5Map = new Map([["entry", variant ? 2 : 1]]);
        break;
    case "6": {
        // One weak-map entry whose key stays reachable.
        const key = {};
        globalThis.h5WeakKey = key;
        globalThis.h5WeakMap = new WeakMap([[key, variant ? 2 : 1]]);
        break;
    }
    case "7": {
        // One private field.
        class H5Private {
            #field;
            constructor(value) {
                this.#field = value;
            }
        }
        globalThis.h5Private = new H5Private(variant ? 2 : 1);
        break;
    }
    case "8":
        // One global let, which the top level sets.
        break;
    }
})(...arguments);

// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer
// jitcache-runs: Producer

// Harness sub-SPEC H5: each pair that H5 names as alike to JavaScript describes alike, so every sequence passes.
// Sequence k makes its Producer run and the oracle's Off run differ in one such way, and the oracle compares their
// end-of-run descriptions. That makes this file the control of describe-differences.js, whose two runs differ in a way
// JavaScript can reach. Sequence 4 meets both forms of its pair inside each run instead, for the reason it gives. Where
// a form depends on what the engine did, the case throws unless the shell shows that the form came about. The shell
// cannot show Math's unread form, as sequence 6 explains.
(function main(role, scratch, artifact, sequence) {
    const variant = role === "Producer";

    // An array of the values 1, 2 and 3 whose storage the first element's first value decides.
    function arrayWith(first, storage) {
        const array = [first, 2, 3];
        array[0] = 1;
        const mode = $vm.indexingMode(array);
        if (!mode.includes(storage))
            throw new Error(`the array has ${mode} storage, not ${storage}`);
        return array;
    }

    // WeakRefs to fresh objects, each reachable only through its WeakRef, and how many of them a collection has emptied.
    // Every target is created and read in a callback, so none stays in main's frame, where the conservative stack scan
    // would find it at every later collection of the run.
    function weakRefsToFreshTargets(count) {
        return Array.from({ length: count }, () => new WeakRef({ target: true }));
    }
    function collectedTargets(refs) {
        return refs.filter(ref => ref.deref() === undefined).length;
    }

    switch (sequence) {
    case "0": {
        // A dictionary object and an ordinary one with the same properties added in the same order. describe prints the
        // object's structure, which names a dictionary's kind.
        const object = {};
        object.first = 1;
        object.second = 2;
        if (variant)
            $vm.toUncacheableDictionary(object);
        if (describe(object).includes("Dictionary") !== variant)
            throw new Error(variant ? "the object is no dictionary" : "the object is a dictionary");
        globalThis.h5Object = object;
        break;
    }
    case "1":
        // An array of the same values with Int32 and Double storage.
        globalThis.h5Array = variant ? arrayWith(1.5, "Double") : arrayWith(0, "Int32");
        break;
    case "2":
        // An array of the same values with Contiguous and Int32 storage.
        globalThis.h5Array = variant ? arrayWith({ }, "Contiguous") : arrayWith(0, "Int32");
        break;
    case "3": {
        // A rope and a flat string.
        let string = "a string long enough that a concatenation makes it a rope;";
        string += " its second half";
        if (variant) {
            if (!isRope(string))
                throw new Error("the concatenation is no rope");
        } else {
            string.charCodeAt(0);
            if (isRope(string))
                throw new Error("reading a character left the rope unresolved");
        }
        globalThis.h5String = string;
        break;
    }
    case "4": {
        // A WeakRef whose target only it reaches, collected or not. A script cannot make the end-of-run walk meet the
        // live form: that walk's collection runs after the job that created the WeakRef has ended and cleared its kept
        // objects, so only the conservative stack scan, which no script controls, could leave such a target alive there.
        // Every run therefore meets both forms inside its job. It describes the WeakRefs while the job keeps their
        // targets, which survive the description's own collection, and again once releaseWeakRefs has cleared the kept
        // objects and a full collection has taken the targets. The scan may keep a few of them, so at least one of the
        // sixteen must lose its target, and the two descriptions must be equal whichever do. The WeakRefs then stay
        // reachable for the end-of-run walk.
        const refs = weakRefsToFreshTargets(16);
        const whileAlive = jitcacheDescribeHeap(...refs);
        if (collectedTargets(refs))
            throw new Error("a target died while the job that created its WeakRef kept it");
        releaseWeakRefs();
        fullGC();
        if (!collectedTargets(refs))
            throw new Error("no target died once the job's kept objects were cleared");
        if (jitcacheDescribeHeap(...refs) !== whileAlive)
            throw new Error("a WeakRef describes differently once its target is collected");
        globalThis.h5WeakRefs = refs;
        break;
    }
    case "5": {
        // A function whose body ran in the LLInt and one whose body ran in baseline code.
        function h5Body(x) {
            return x + 1;
        }
        const calls = variant ? 1000 : 1;
        for (let i = 0; i < calls; ++i)
            h5Body(i);
        const compiles = jitcacheBodyEvents(h5Body, "call").baselineCompiles;
        if (variant ? !compiles : compiles)
            throw new Error(`the function made ${compiles} baseline compiles after ${calls} calls`);
        globalThis.h5Function = h5Body;
        break;
    }
    case "6":
        // Math, read before the end of the run or never. Math is a lazy property of the global object, which the walk
        // reifies, and nothing in the shell reads it. No check shows the unread form, because any read of Math reifies
        // it and a script can hand describe only the global object's proxy.
        if (variant && typeof Math.max !== "function")
            throw new Error("Math.max is no function");
        break;
    case "7":
        // A value only the shell's arguments reaches.
        globalThis.arguments.push({ onlyTheArgumentsReachThis: variant ? 2 : 1 });
        break;
    }
})(...arguments);

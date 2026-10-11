// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer

// SPEC-cb.md section 11.3, argument profiles (P2) in functions and constructors. A function called with several types,
// a function used both as a constructor and through Function.prototype.call, whose two specializations are two bodies,
// and a class constructor each learn argument predictions in the Producer. In twins builds the Producer checks that its
// committed sections predict every declared argument after a delta that follows a full collection, whose finalization
// drains the argument profiles; the Consumer checks that each body was imported at its first call or construction.
(function main(role) {
    function mix(a, b, c) {
        return typeof a === "number" ? a + (b | 0) : `${a}${c}`.length;
    }
    function Point(x, y) {
        this.x = x;
        this.y = y;
    }
    class Box {
        constructor(value, label) {
            this.value = value;
            this.label = label;
        }
    }

    const inputs = [[1, 2.5, "s"], ["text", null, 3], [{}, true, undefined], [4.25, "7", [1]]];
    const target = {};
    let total = 0;
    for (let i = 0; i < 150; ++i) {
        const [a, b, c] = inputs[i & 3];
        total += mix(a, b, c);
        const point = i & 1 ? new Point(i, i + 0.5) : new Point(`p${i & 7}`, [i]);
        total += typeof point.x === "number" ? point.x : point.y.length;
        Point.call(target, i & 3, null);
        total += target.x;
        const box = i & 1 ? new Box(i * 0.5, "b") : new Box(null, i);
        total += typeof box.value === "number" ? box.value : box.label;
    }
    print(total);

    if (role === "Consumer" && typeof jitcacheBodyEvents === "function") {
        const sections = load("./resources/cb-sections.js", "caller relative");
        sections.expectImported(mix, "call", "mix");
        sections.expectImported(Point, "construct", "new Point");
        sections.expectImported(Point, "call", "Point.call");
        sections.expectImported(Box, "construct", "new Box");
        return;
    }
    if (role !== "Producer" || typeof jitcacheDelta !== "function")
        return;

    fullGC();
    jitcacheDelta();
    const sections = load("./resources/cb-sections.js", "caller relative");
    for (const [fn, kind, label, declared] of [[mix, "call", "mix", 3], [Point, "construct", "new Point", 2], [Point, "call", "Point.call", 2], [Box, "construct", "new Box", 2]]) {
        const state = sections.checkBody(fn, kind, label).state;
        if (state.header.numArguments !== declared + 1)
            throw new Error(`${label}: cb.state holds ${state.header.numArguments} argument predictions, expected ${declared + 1}`);
        // Argument 0 is `this`; every declared argument saw a value.
        for (let index = 1; index <= declared; ++index) {
            if (!state.arguments[index])
                throw new Error(`${label}: argument ${index} carries no prediction`);
        }
    }
})(...arguments);

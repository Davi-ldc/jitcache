// jitcache-requires: twins
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-expect-fault: 1:1 ucb.identity

// The holder check for a child UFE that an imported parent's decode made. A capture lists, in its ucb.identity, the
// holder digest of each child UFE of the UCB it captures, computed from the producer's own UFE; a consumer that imports
// the parent keeps the list, and a request for one of those children compares the listed digest with its body's
// holderDigest instead of encoding the UFE. Strict, which every run here has, still computes the UFE's own digest,
// decides by it, and requires the listed digest to equal it.
// The Producer runs vhOuter and the vhInner closures it returns until both are captured.
// - Sequence 0: the Consumer imports vhOuter, then vhInner, whose request has the digest vhOuter's body lists
//   (vouchedHolderChecks) and computes its own (holderDigests), and the two agree, so vhInner imports.
// - Sequence 1: the Producer flips a byte of the digest vhOuter's body lists for vhInner. The Consumer imports vhOuter;
//   vhInner's request finds the listed digest unequal to the UFE's own, which is invalid material, so cache activity turns
//   off at ucb.identity, vhInner is generated natively, and the results still equal the JITCache-off run.
load("./resources/ucb.js", "caller relative");
load("./resources/sections.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const rounds = 1000;

    function vhOuter(n)
    {
        const base = (n * 3) | 0;
        function vhInner(k)
        {
            let total = (base + k) | 0;
            for (let i = 0; i < 4; ++i)
                total = (total * 3 + i) | 0;
            return total;
        }
        return vhInner;
    }
    const expectedOf = (n, k) => {
        let total = (((n * 3) | 0) + k) | 0;
        for (let i = 0; i < 4; ++i)
            total = (total * 3 + i) | 0;
        return total;
    };

    // Counts the requests of `call`: statistics read around it, since a window's own action is a request too.
    function counted(label, call, expectations)
    {
        const before = t.statistics();
        const value = call();
        const after = t.statistics();
        if (t.imports)
            t.expect(label, t.delta(before, after), expectations);
        return value;
    }

    if (t.role === "Producer") {
        let total = 0;
        for (let i = 0; i < rounds; ++i)
            total = (total + vhOuter(i & 7)(i & 3)) | 0;
        t.check(total === (() => {
            let sum = 0;
            for (let i = 0; i < rounds; ++i)
                sum = (sum + expectedOf(i & 7, i & 3)) | 0;
            return sum;
        })(), "the Producer's sum differs");
        if (t.twins && t.sequence === 1) {
            const key = t.bodyKey(vhOuter);
            const bytes = t.readSection(key, "ucb.identity");
            t.check(bytes, `no ucb.identity for ${key}`);
            const identity = ucbSections.identity(bytes);
            t.check(identity.childHolderDigestCount === 1, () => `vhOuter's body lists ${identity.childHolderDigestCount} child holder digests`);
            const at = identity.childHolderDigests;
            t.rewriteSection(key, "ucb.identity", at, [bytes[at] ^ 0x5a]);
        }
        print("vouched holder");
        t.finish();
        return;
    }

    const inner = counted("vhOuter's first call", () => vhOuter(5), { imports: 1 });
    if (t.sequence === 0) {
        const value = counted("vhInner's first call", () => inner(2), {
            imports: 1, vouchedHolderChecks: 1, holderDigests: 1, "misses.Holder": 0,
        });
        t.check(value === expectedOf(5, 2), () => `vhInner(2) gave ${value}`);
        t.check(t.status() === null, () => `cache activity turned off at ${t.status()}`);
    } else {
        const value = inner(2);
        t.check(value === expectedOf(5, 2), () => `vhInner(2) gave ${value}`);
        if (t.imports)
            t.check(t.status() === "ucb.identity", () => `cache activity is off at ${t.status()}, expected ucb.identity`);
    }
    for (let i = 0; i < rounds; ++i) {
        const value = vhOuter(i & 7)(i & 3);
        t.check(value === expectedOf(i & 7, i & 3), () => `vhOuter(${i & 7})(${i & 3}) gave ${value}`);
    }
    print("vouched holder");
    t.finish();
})(...arguments);

// jitcache-requires: twins
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; ConsumerProducer
// jitcache-expect-no-install: 1

// SPEC-ucb.md section 13.3, holder-miss.js, check C11 (sections 3.4 and 4.4): a function body whose holderDigest is
// rewritten through jitcacheRewriteSection (SPEC-integrator.harness.md section 5.2) fits no UFE, so every request that
// reaches C11 misses Holder, stamps the miss, and leaves cache activity on; the run equals the JITCache-off run.
// The function hmF is declared by a program evaluated in realms of its own (runString), so each realm's first call of
// hmF is a request for the one UCB the program's CodeCache entry holds.
// - Both Producers run hmF until it is captured and then rewrite the first byte of its body's holderDigest.
// - Sequence 0: the Consumer's import of hmF misses Holder and generates; a later request in another realm finds the
//   miss stamped at the index token it still sees and reads nothing (misses.BodyUnchanged).
// - Sequence 1: the ConsumerProducer's import misses Holder and generates a UCB that stays in the LLInt; the run then
//   rewrites the body again, which commits it under a new index token, so the next request in another realm attaches to
//   the generated UCB, matches it, and misses Holder there too (section 7.3.2 step 7); the request after it reads nothing.
// hmF never installs, so each importing run declares jitcache-expect-no-install.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const programText = "function hmF(x) { let total = x; for (let i = 0; i < 4; ++i) total = (total * 3 + i) | 0; return total; }";
    const expectedOf = x => {
        let total = x;
        for (let i = 0; i < 4; ++i)
            total = (total * 3 + i) | 0;
        return total;
    };
    const holderDigestOffset = 112;

    // Calls hmF of a new realm `count` times; returns its key, which the realm's CodeBlock gives.
    function callInNewRealm(count)
    {
        const realm = runString(programText);
        for (let i = 0; i < count; ++i) {
            const value = realm.hmF(i);
            t.check(value === expectedOf(i), () => `hmF(${i}) gave ${value}`);
        }
        return t.bodyKey(realm.hmF);
    }

    function rewriteHolderDigest(key, at)
    {
        const identity = t.readSection(key, "ucb.identity");
        t.check(identity, `no ucb.identity for ${key}`);
        t.rewriteSection(key, "ucb.identity", at, [identity[at] ^ 0x5a]);
    }

    if (t.role === "Producer") {
        const key = callInNewRealm(100);
        if (t.twins)
            rewriteHolderDigest(key, holderDigestOffset);
        print("holder miss");
        t.finish();
        return;
    }

    // The Producer ran these helpers hot, so their first calls import; they happen here, outside the measured windows.
    t.check(expectedOf(0) === 18, "expectedOf(0) is not 18");
    t.bodyKey(expectedOf);

    const key = t.window("hmF's import", () => callInNewRealm(3), {
        "misses.Holder": 1, imports: 0, "records.Generated": [">=", 1], holderDigests: 1,
    });
    t.check(t.status() === null || t.status() === undefined, () => `cache activity turned off at ${t.status()}`);

    if (t.role === "ConsumerProducer") {
        rewriteHolderDigest(key, holderDigestOffset + 1);
        t.window("hmF's attach to its generated UCB", () => callInNewRealm(3), {
            "misses.Holder": 1, attaches: 0, holderDigests: 1, contextDigests: 1, sourceDigests: 0,
        });
    }
    t.window("hmF's request at an unchanged index token", () => callInNewRealm(3), {
        "misses.BodyUnchanged": 1, "misses.Holder": 0, attaches: 0, contextDigests: 0, holderDigests: 0, sourceDigests: 0,
    });
    t.check(t.status() === null || t.status() === undefined, () => `cache activity turned off at ${t.status()}`);
    print("holder miss");
    t.finish();
})(...arguments);

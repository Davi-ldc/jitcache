// jitcache-requires: twins
// jitcache-runs: Producer; ConsumerProducer
// jitcache-require-fault: 1 ucb.strict-core
// jitcache-expect-no-install: 1

// A body whose coreDigest differs from the encoding of a live generated UCB it is matched against, rewritten through
// jitcacheRewriteSection. The Producer captures srF, declared by a program evaluated in realms of its own (runString),
// and rewrites its body's context digest, so that the ConsumerProducer's import misses Context and generates a UCB that
// stays in the LLInt, stamped at that index token. The ConsumerProducer then rewrites the body once more, restoring the
// context digest and changing the first byte of coreDigest, which commits it under a new token, so the next realm's
// request attaches to the generated UCB and matches it by its generation inputs. With strict on, the strict check
// compares the UCB's core encoding with coreDigest and finds invalid material at ucb.strict-core, so the body never
// installs there; the engine's C++ tests check that the same attach succeeds with strict off. Every run equals the
// JITCache-off run.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const contextDigestOffset = 48;
    const coreDigestOffset = 80;

    const programText = "function srF(x) { let total = x; for (let i = 0; i < 4; ++i) total = (total * 7 + i) | 0; return total; }";
    const expectedF = x => {
        let total = x;
        for (let i = 0; i < 4; ++i)
            total = (total * 7 + i) | 0;
        return total;
    };

    function callInNewRealm(count)
    {
        const realm = runString(programText);
        for (let i = 0; i < count; ++i) {
            const value = realm.srF(i);
            t.check(value === expectedF(i), () => `srF(${i}) gave ${value}`);
        }
        return t.bodyKey(realm.srF);
    }

    function flip(key, offset)
    {
        const identity = t.readSection(key, "ucb.identity");
        t.check(identity, `no ucb.identity for ${key}`);
        return identity[offset] ^ 0x5a;
    }

    if (t.role === "Producer") {
        const key = callInNewRealm(100);
        if (t.twins)
            t.rewriteSection(key, "ucb.identity", contextDigestOffset, [flip(key, contextDigestOffset)]);
        print("strict reuse");
        t.finish();
        return;
    }

    // The helpers the Producer ran hot import at their first calls, here, outside the measured windows.
    t.check(expectedF(0) === 66, "expectedF(0) is not 66");
    t.bodyKey(expectedF);

    const key = t.window("srF's import", () => callInNewRealm(3), { "misses.Context": 1, imports: 0 });
    if (t.twins) {
        // The context digest's 32 bytes as the Producer captured them, then a changed first byte of coreDigest.
        const identity = t.readSection(key, "ucb.identity");
        const bytes = Array.from(identity.subarray(contextDigestOffset, coreDigestOffset + 1));
        bytes[0] ^= 0x5a;
        bytes[coreDigestOffset - contextDigestOffset] ^= 0x5a;
        t.rewriteSection(key, "ucb.identity", contextDigestOffset, bytes);
    }
    t.window("srF's attach under strict", () => callInNewRealm(3), { "invalidMaterial.StrictCore": 1, attaches: 0 });
    t.check(!t.configured || t.status() === "ucb.strict-core", () => `cache activity is off at ${t.status()}, expected ucb.strict-core`);
    print("strict reuse");
    t.finish();
})(...arguments);

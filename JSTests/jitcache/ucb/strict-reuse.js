// jitcache-requires: twins
// jitcache-runs: Producer; ConsumerProducer
// jitcache-runs: Producer; ConsumerProducer --jitcache-strict=0
// jitcache-runs: Off --diskCachePath=.; Producer; Consumer --diskCachePath=.
// jitcache-runs: Off --diskCachePath=.; Producer; Consumer --diskCachePath=. --jitcache-strict=0
// jitcache-require-fault: 0:1 ucb.strict-core
// jitcache-expect-no-install: 0:1
// jitcache-expect-no-install: 2:2
// jitcache-expect-no-install: 3:2
// jitcache-pin: off sequences 2 and 3 decode the jsc shell's bytecode cache, whose payload computeJSCBytecodeCacheVersion ties to this build, so the pin's build rejects it and generates where this build decodes

// SPEC-ucb.md section 13.3, strict-reuse.js, sections 7.3.2 and 10.2: a body whose coreDigest differs from the encoding
// of a live UCB it is matched against, rewritten through jitcacheRewriteSection (SPEC-integrator.harness.md section 5.2).
// - Sequences 0 and 1, a live generated UCB. The Producer captures srF, declared by a program evaluated in realms of its
//   own (runString), and rewrites its body's context digest, so that the ConsumerProducer's import misses Context and
//   generates a UCB that stays in the LLInt, stamped at that index token. The ConsumerProducer then rewrites the body
//   once more, restoring the context digest and changing the first byte of coreDigest, which commits it under a new
//   token, so the next realm's request attaches to the generated UCB and matches it by its generation inputs. With strict
//   on, S1 compares the UCB's core encoding with coreDigest and finds invalid material at ucb.strict-core (sequence 0);
//   with strict off nothing compares them, and the request attaches and its newborn CodeBlock installs the body
//   (sequence 1).
// - Sequences 2 and 3, a live decoded UCB. The first run, with JITCache off, calls and constructs srG, so the jsc shell's
//   bytecode cache (--diskCachePath; the runs work in the sequence's scratch directory) holds both its bodies. The
//   Producer generates both, captures them and changes the first byte of the construct body's coreDigest. The Consumer
//   decodes srG at its first call, seeds the call body and records the construct body as decoded; `new srG()` meets that
//   live decoded UCB, whose match compares its core encoding with coreDigest in both modes, and misses CoreDigest with
//   strict on (sequence 2) and off (sequence 3).
// The body never installs where strict finds the invalid material or where its live UCB is decoded, so those runs
// declare jitcache-expect-no-install. Every run equals the JITCache-off run.
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
    function srG(x)
    {
        if (new.target) {
            this.value = (x * 2 + 1) | 0;
            return;
        }
        return (x * 3 + 2) | 0;
    }

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

    if (t.sequence < 2) {
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
        if (t.sequence === 0) {
            t.window("srF's attach under strict", () => callInNewRealm(3), { "invalidMaterial.StrictCore": 1, attaches: 0 });
            t.check(!t.configured || t.status() === "ucb.strict-core", () => `cache activity is off at ${t.status()}, expected ucb.strict-core`);
        } else {
            const installs = t.progress()?.installs;
            t.window("srF's attach with strict off", () => callInNewRealm(3), { attaches: 1, "misses.CoreDigest": 0 });
            t.check(!t.configured || t.progress().installs === installs + 1, "srF's attached body was not installed");
            t.check(!t.configured || t.status() === null, () => `cache activity turned off at ${t.status()}`);
        }
        print("strict reuse");
        t.finish();
        return;
    }

    // Sequences 2 and 3.
    let total = 0;
    if (t.role === "Producer") {
        for (let i = 0; i < 100; ++i)
            total = (total + srG(i) + new srG(i).value) | 0;
        const key = t.bodyKey(srG, "construct");
        if (t.twins)
            t.rewriteSection(key, "ucb.identity", coreDigestOffset, [flip(key, coreDigestOffset)]);
    } else {
        total = t.window("srG's first call, which decodes both of its slots", () => srG(0), { seededDecodes: [">=", 1], "records.Decoded": [">=", 1] });
        total = (total + t.window("new srG() on its live decoded construct body", () => new srG(0), {
            "misses.CoreDigest": 1, attaches: 0,
        }).value) | 0;
        for (let i = 1; i < 100; ++i)
            total = (total + srG(i) + new srG(i).value) | 0;
        t.check(!t.configured || t.status() === null, () => `cache activity turned off at ${t.status()}`);
    }
    // Sum over i of (3i + 2) + (2i + 1), for i from 0 to 99.
    t.check(total === 5 * 4950 + 300, `srG's calls and constructions added up to ${total}`);
    print("strict reuse");
    t.finish();
})(...arguments);

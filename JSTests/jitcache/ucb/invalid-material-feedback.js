// jitcache-requires: twins
// jitcache-expect-fault: 1 ucb.feedback
// jitcache-expect-no-install: 1
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer

// SPEC-ucb.md section 13.3, invalid-material.js, for the rules of section 5.2: with strict on, a body whose ucb.feedback
// breaks one is invalid material at ucb.feedback, cache activity turns off, and the run goes on as the JITCache-off run
// does. resources/invalid-material-cases.js runs each case as invalid-material.js does, which holds the other sections'
// cases; the runner reads directives only in a script's first 50 lines, which the two tables would not fit together.
//
// Each sequence above is one case of the table below, which the sequence index picks: every rule of section 5.2 that a
// rewrite of the same size can break. In order, the header (the magic, the layout version, the reserved bytes, a count of
// 1 << 28), each profile (a prediction carrying SpecInt52Any, array modes outside ALL_ARRAY_MODES, an array flag past the
// eight, the pruning flag, binary and unary bits out of range), the gap before the exit sites, each exit site field (an
// unset kind and one past the last, a JIT type and an inline kind out of range, a nonzero padding byte, two equal sites),
// the tier-up bytes (the three TriStates and byte 39), the LLInt counter (a negative threshold, a total that is not finite,
// a progress below 0 and one of 2^31), a child bit of 2, a constant bit at C, a nonzero byte of final padding, and a
// constant count other than the identity section's. A threshold of INT32_MAX with a progress in range obeys section 5.2,
// which accepts any threshold of at least 0 that way. No same-size rewrite can give the section the wrong size; SPEC-ucb.md
// U5 checks that rule.
load("./resources/ucb.js", "caller relative");
load("./resources/sections.js", "caller relative");
load("./resources/invalid-material-cases.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    invalidMaterialCases.run(ucbTest(role, scratch, artifact, sequence), 29, c => {
        // The LLInt counter's three fields at bytes 40 to 51: an armed threshold and the given total and counter.
        const counter = (total, value) => [40, [...c.S.le32(1000), ...c.float32Bytes(total), ...c.S.le32(value)]];
        return [
            c.feedbackCase("the feedback magic", feedback => [0, [feedback.bytes[0] ^ 1]]),
            c.feedbackCase("the feedback layout version", () => [4, [2, 0]]),
            c.feedbackCase("a nonzero reserved byte", () => [6, [1]]),
            c.feedbackCase("a value profile count of 1 << 28", () => [8, c.S.le32(1 << 28)]),
            c.feedbackCase("a prediction carrying SpecInt52Any", feedback => {
                c.t.check(feedback.counts.valueProfiles, "imPlain has no value profile");
                return [feedback.predictions + 4, [feedback.bytes[feedback.predictions + 4] | 0x80]];
            }),
            c.feedbackCase("array modes outside ALL_ARRAY_MODES", feedback => {
                c.t.check(feedback.counts.arrayProfiles, "imPlain has no array profile");
                // Bit 14 stands for indexing mode 0x0E, which no indexing type has.
                return [feedback.arrays + 1, [feedback.bytes[feedback.arrays + 1] | 0x40]];
            }),
            c.feedbackCase("an array profile flag past the eight", feedback => {
                c.t.check(feedback.counts.arrayProfiles, "imPlain has no array profile");
                return [feedback.arrays + 5, [feedback.bytes[feedback.arrays + 5] | 0x01]];
            }),
            c.feedbackCase("an array profile with DidPerformFirstRunPruning", feedback => {
                c.t.check(feedback.counts.arrayProfiles, "imPlain has no array profile");
                return [feedback.arrays + 4, [feedback.bytes[feedback.arrays + 4] | 0x40]];
            }),
            c.feedbackCase("binary arithmetic bits from 1 << 14", feedback => {
                c.t.check(feedback.counts.binaryArithProfiles, "imPlain has no binary arithmetic profile");
                return [feedback.binary + 1, [feedback.bytes[feedback.binary + 1] | 0xc0]];
            }),
            c.feedbackCase("unary arithmetic bits from 1 << 10", feedback => {
                c.t.check(feedback.counts.unaryArithProfiles, "imPlain has no unary arithmetic profile");
                return [feedback.unary, [0xff, 0xff]];
            }),
            c.feedbackCase("a nonzero byte in the gap before the exit sites", feedback => [feedback.unary + 2 * feedback.counts.unaryArithProfiles, [1]], c.gapBeforeExits),
            c.feedbackCase("an exit site of ExitKindUnset", feedback => {
                c.t.check(feedback.counts.exitSites, "imExit has no exit site");
                return [feedback.exits + 4, [0]];
            }, "imExit"),
            c.feedbackCase("an exit kind past the last", feedback => {
                c.t.check(feedback.counts.exitSites, "imExit has no exit site");
                return [feedback.exits + 4, [200]];
            }, "imExit"),
            c.feedbackCase("an exiting JIT type other than ExitFromDFG and ExitFromFTL", feedback => {
                c.t.check(feedback.counts.exitSites, "imExit has no exit site");
                return [feedback.exits + 5, [0]];
            }, "imExit"),
            c.feedbackCase("an exiting inline kind other than ExitFromNotInlined and ExitFromInlined", feedback => {
                c.t.check(feedback.counts.exitSites, "imExit has no exit site");
                return [feedback.exits + 6, [3]];
            }, "imExit"),
            c.feedbackCase("a nonzero exit site padding byte", feedback => {
                c.t.check(feedback.counts.exitSites, "imExit has no exit site");
                return [feedback.exits + 7, [1]];
            }, "imExit"),
            c.feedbackCase("two equal exit sites", feedback => {
                c.t.check(feedback.counts.exitSites >= 2, () => `imExits has ${feedback.counts.exitSites} exit sites`);
                return [feedback.exits + 8, feedback.bytes.subarray(feedback.exits, feedback.exits + 8)];
            }, "imExits"),
            c.feedbackCase("a didOptimize of 3", () => [36, [3]]),
            c.feedbackCase("a quick DFG tier-up of 3", () => [37, [3]]),
            c.feedbackCase("a quick FTL tier-up of 2", () => [38, [2]]),
            c.feedbackCase("a nonzero byte 39", () => [39, [1]]),
            c.feedbackCase("a negative LLInt threshold", () => [40, [0xff, 0xff, 0xff, 0xff]]),
            c.feedbackCase("an LLInt total that is not finite", () => counter(Infinity, 0)),
            c.feedbackCase("an LLInt progress below 0", () => counter(0, -1)),
            c.feedbackCase("an LLInt progress of 2^31", () => counter(2 ** 31, 0)),
            c.feedbackCase("a child bit of 2", feedback => {
                c.t.check(feedback.counts.functionDecls + feedback.counts.functionExprs, "imPlain has no child");
                return [feedback.children, [2]];
            }),
            c.feedbackCase("a constant bit at C", feedback => c.setBit(feedback.bytes, feedback.constants, feedback.counts.constants), c.unevenConstants),
            c.feedbackCase("a nonzero byte of final padding", feedback => [feedback.end, [1]], c.paddedFeedback),
            c.feedbackCase("a constant count other than the identity section's", feedback => [52, c.S.le32(feedback.counts.constants + 1)]),
        ];
    });
})(...arguments);

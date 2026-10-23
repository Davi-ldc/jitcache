// A module whose top level loops as long as its run says: a few rounds, which never compile, except in the
// ConsumerProducer of module-analysis-recapture.mjs, whose rounds compile the top level. What it exports does not depend
// on the number of rounds.
const maRounds = process.argv[2] === "ConsumerProducer" ? 20000 : 3;
let maTotal = 0;
for (let i = 0; i < maRounds; ++i)
    maTotal = (maTotal + i * 3) & 0xffff;
export const maRecaptured = maTotal >= 0 ? "settled" : "negative";

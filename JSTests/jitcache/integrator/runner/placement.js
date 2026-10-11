// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer

// Harness sub-SPEC H2, the 50 sequences above, which fill the directive lines: in the environment the runner's
// calibration chose, every Consumer's executable pool lies outside its Producer's, its structure reservation has another
// base, and each of its three heap probes differs from its Producer's. Each process writes its layout file before its
// script runs (harness sub-SPEC section 4), so the Consumer reads both files and throws on a placement that did not
// move, which fails the run. The Off runs of the oracle record no layout and check nothing.
load("./resources/warm-up.js", "caller relative");
(function main(role, scratch) {
    print(warmUp());
    if (role !== "Consumer")
        return;

    // pool <start> <end>, structures <start> <size> and heap <vm> <cell> <atom>, in hex.
    function layout(path) {
        const lines = {};
        for (const line of readFile(path).split("\n")) {
            const [name, ...values] = line.trim().split(/\s+/);
            if (name)
                lines[name] = values.map(value => BigInt(`0x${value}`));
        }
        for (const name of ["pool", "structures", "heap"]) {
            if (!lines[name])
                throw new Error(`${path} holds no ${name} line`);
        }
        return lines;
    }
    const hex = value => `0x${value.toString(16)}`;
    const producer = layout(`${scratch}/run0.layout`);
    const consumer = layout(`${scratch}/run1.layout`);

    const [producerStart, producerEnd] = producer.pool;
    const [consumerStart, consumerEnd] = consumer.pool;
    if (consumerStart < producerEnd && producerStart < consumerEnd)
        throw new Error(`the Consumer's executable pool [${hex(consumerStart)}, ${hex(consumerEnd)}) overlaps the Producer's [${hex(producerStart)}, ${hex(producerEnd)})`);
    if (consumer.structures[0] === producer.structures[0])
        throw new Error(`the Consumer's structure reservation has the Producer's base ${hex(producer.structures[0])}`);
    ["vm", "cell", "atom"].forEach((probe, index) => {
        if (consumer.heap[index] === producer.heap[index])
            throw new Error(`the Consumer's ${probe} probe repeats the Producer's ${hex(producer.heap[index])}`);
    });
})(...arguments);

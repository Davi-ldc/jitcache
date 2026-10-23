# basics.md: the startup deferral scale in the tier-up threshold

Doc: `skills/reference/basics.md`, section "Tier-up", second and third paragraphs.

## Passage

> When the counter reaches zero or more, execution falls into the tier-up handler, which compares the executions counted so far with a corrected threshold: the base multiplied by `available / (available − allocated − estimate)`, then reduced by `min(initial threshold, slice ceiling) / 2`.

## What changed

`ExecutionCounter::checkIfThresholdCrossedAndSet` takes a `startupDeferralScale`, which `hasCrossedThreshold` and `setThreshold` multiply into the memory-corrected threshold. The counter's fields keep their meaning.

- The scale is `VM::startupJITDeferralScale()`, 1 by default. The VM constructor takes it from `Options::startupJITDeferralScale` when that is above 1, and an embedder sets it with `VM::setStartupJITDeferralScale`; any value not above 1 ends the window and resets it to 1.
- It applies to LLInt→baseline (`CodeBlock::checkIfJITThresholdReached`, except for a CB already in the baseline) and baseline→DFG (`CodeBlock::checkIfOptimizationThresholdReached`, except once the CB has an optimized replacement, where the counter only spaces OSR-entry retries). DFG→FTL passes no scale.
- While the scale is not 1, `setThreshold` arms `min(threshold, max(unscaled threshold, clipped threshold))` instead of the clipped threshold, so the counter checks again within one unscaled period. `setStartupJITDeferralScale` therefore walks no CB when the window ends: each counter sees scale 1 on its next check.

## Proposed text

After the sentence quoted above, add:

> During the VM's startup window, LLInt→baseline and baseline→DFG multiply that threshold by `VM::startupJITDeferralScale()` (option `startupJITDeferralScale`, or set by the embedder; 1 ends the window), and the armed slice never exceeds one unscaled threshold, so ending the window takes effect at each counter's next check.

Net size change is about +300 bytes; the doc is at 39,973, so condense elsewhere first.

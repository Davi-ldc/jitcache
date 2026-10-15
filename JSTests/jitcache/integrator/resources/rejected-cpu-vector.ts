// cpu-vector.js's checker for its Consumer: start found the Producer's header incompatible at a CPU feature bit, one of
// the three predicates whose bits differ between QEMU's max and neoverse-n1 models, which the detail names by the
// lowest differing bit.
import { checkStartResult } from "./checker.ts";

checkStartResult("rejected", "start.incompatible", [
  "supportsDoubleToInt32ConversionUsingJavaScriptSemantics",
  "supportsRoundFloatToIntegerFloat",
  "supportsSHA3",
]);

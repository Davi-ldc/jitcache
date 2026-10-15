// producer-kill.js's checker after a kill from mid-stream to after-index, which leaves the killed commit's bytes past the
// pack's valid end.
import { checkBytesPastValidEnd } from "./checker.ts";

checkBytesPastValidEnd(true);

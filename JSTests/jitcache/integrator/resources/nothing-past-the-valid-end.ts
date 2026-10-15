// producer-kill.js's checker after a kill before-append, when the commit wrote nothing, or after-root, when its bytes are
// committed.
import { checkBytesPastValidEnd } from "./checker.ts";

checkBytesPastValidEnd(false);

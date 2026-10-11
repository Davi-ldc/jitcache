// producer-kill.js's checker for the clean after a kill before-create, when no temporary exists yet, or after-rename,
// when the temporary is the published body (harness sub-SPEC section 12).
import { checkCleanRemoved } from "./checker.ts";

checkCleanRemoved(0);

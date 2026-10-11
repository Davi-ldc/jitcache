// producer-kill.js's checker for the clean after a kill from after-create to after-reread, which leaves the commit's
// temporary in cache/ (harness sub-SPEC section 12).
import { checkCleanRemoved } from "./checker.ts";

checkCleanRemoved(1);

// producer-kill.js's checker for the clean after a kill before-append, when the commit wrote nothing, or after-root, when
// its bytes are committed.
import { checkCleanTruncated } from "./checker.ts";

checkCleanTruncated(false);

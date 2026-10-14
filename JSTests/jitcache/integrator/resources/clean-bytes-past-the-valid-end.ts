// producer-kill.js's checker for the clean after a kill from mid-stream to after-index, which leaves the killed commit's
// bytes past the pack's valid end.
import { checkCleanTruncated } from "./checker.ts";

checkCleanTruncated(true);

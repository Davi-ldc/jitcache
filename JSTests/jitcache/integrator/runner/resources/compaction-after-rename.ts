// kill-points.js's checker for a kill right after a compaction's rename: the new pack lists every body and is not
// retired, and no temporary is left.
import { checkPackState } from "./temporaries.ts";

checkPackState({ atLeastBodies: 1, retired: false, temporaries: 0 });

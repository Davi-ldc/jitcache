// kill-points.js's checker for a kill between a compaction's retired mark and its rename: the old pack, retired, still
// lists every body, and the new pack waits beside it as a temporary, which clean removes.
import { checkPackState } from "./temporaries.ts";

checkPackState({ atLeastBodies: 1, retired: true, temporaries: 1 });

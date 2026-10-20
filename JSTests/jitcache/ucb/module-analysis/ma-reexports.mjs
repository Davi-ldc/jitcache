// Re-exports of an imported binding, of a namespace import and of a deferred namespace import.
import { maAlpha } from "./ma-names.mjs";
import * as maNamesNamespace from "./ma-names.mjs";
import defer * as maDeferredNamespace from "./ma-deferred.mjs";

export { maAlpha as maImportedBinding, maNamesNamespace, maDeferredNamespace };

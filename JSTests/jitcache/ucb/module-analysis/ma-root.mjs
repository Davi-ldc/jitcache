// The graph module-analysis.mjs and module-analysis-worker.mjs import: together with the modules it imports, one form of
// a module's analysis each. A default import of an expression, of a named function and of an anonymous class; named
// imports with and without an alias; a namespace import; a deferred namespace import; a bare import; json, and two
// host-defined types on one specifier; a local exported under several names, string export names, an indirect export,
// the same star export twice and a namespace star export (ma-exports.mjs); re-exports of an imported binding, of a
// namespace import and of a deferred one (ma-reexports.mjs); import.meta (ma-meta.mjs); top-level awaits
// (ma-await.mjs); more than 255 names, whose ids and blob offsets take two bytes (ma-wide.mjs); a name with a unit
// above 0xFF and a Latin-1 name with one above 0x7F (ma-unicode.mjs). The export named toml shares its string with the
// host-defined type toml. 19 JavaScript modules in all, this one included.
import maDefault from "./ma-default-expression.mjs";
import maNamedFunction from "./ma-default-function.mjs";
import MaAnonymousClass from "./ma-default-class.mjs";
import { maAlpha, maBeta as maRenamed } from "./ma-names.mjs";
import * as maNames from "./ma-names.mjs";
import defer * as maLazy from "./ma-lazy.mjs";
import "./ma-side.mjs";
import maData from "./ma-data.json" with { type: "json" };
import maConfigText from "./ma-config.toml" with { type: "text" };
import maConfig from "./ma-config.toml" with { type: "toml" };
import * as maExports from "./ma-exports.mjs";
import * as maReexports from "./ma-reexports.mjs";
import { maMetaURL } from "./ma-meta.mjs";
import { maSettled, maSum } from "./ma-await.mjs";
import * as maWide from "./ma-wide.mjs";
import * as maUnicode from "./ma-unicode.mjs";
import { maHot } from "./ma-hot.mjs";

export const toml = maConfig;
export { maDefault, maNamedFunction, MaAnonymousClass, maAlpha, maRenamed, maNames, maLazy, maData, maConfigText, maExports,
    maReexports, maMetaURL, maSettled, maSum, maWide, maUnicode, maHot };

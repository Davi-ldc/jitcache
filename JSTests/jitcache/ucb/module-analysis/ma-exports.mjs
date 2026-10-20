// A local exported under several names, one of them a string; an indirect export; the same star export twice; a
// namespace star export.
const maShared = 7;
export { maShared, maShared as maAlias, maShared as "ma string name" };
export { maAlpha as maFromNames } from "./ma-names.mjs";
export * from "./ma-star.mjs";
export * from "./ma-star.mjs";
export * as maStarNamespace from "./ma-star.mjs";

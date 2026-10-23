// A TypeScript module that re-exports a name its source lacks. The record the analysis parse builds keeps its TypeScript
// flag clear, so linking it fails. module-analysis-errors.mjs's Producer links a copy of this text beside an exporter
// that has the name, so that it captures the body with its analysis.
export { maLacking } from "./ma-err-exporter.mjs";

// The bench report a process writes under --jitcache-bench-report, one JSON event per line, and what the benches derive
// from it: a process's spans, their minimum and median over copies, and the installation bound per body, which sets each
// body's import cost against its native compilation's. The runner's bench mode (microbench.ts) and the bound's own
// warmed bench (PerformanceTests/JITCache/bound.ts) both read reports through it.

import { readText } from "./build.ts";

// The values installationBoundSmallestBody may take, in bytes of the installed image's linked size, the install event's
// image.codeSize.
export const smallestBodyCandidates = [0, 128, 256, 512, 1024, 2048, 4096];

// The flush events carry the process's totals so far, so the last line of each holds the whole run; every other event is
// written once per occurrence.
const flushEvents = new Set(["lookup", "index", "budget", "process"]);

// The fields that hold thread CPU time, by event: the only spans the bench takes a minimum and a median of, since the
// minimum filters interference, which only lengthens a span. Every other field is reported by copy: a count, size, score
// or flag has no interference to filter, and a wall time, such as the install steps', only explains a CPU-time span.
const cpuTimeFields: Record<string, (field: string) => boolean> = {
  start: field => field === "nanoseconds",
  open: field => field === "nanoseconds",
  lookup: field => field.endsWith(".nanoseconds"),
  request: field => field === "counted" || field === "apart",
  install: field => field === "total" || field === "bodyRelease",
  compile: field => field === "compileNanoseconds" || field === "finalizeNanoseconds",
  capture: field => /^(scoring|build)\./.test(field) || /^writer\.(.+\.stream|reread|publish)$/.test(field),
  delta: field => field === "phase1Nanoseconds" || field === "phase2Nanoseconds" || field === "phase3Nanoseconds",
  pack: field => field === "compactionNanoseconds",
};

export const isCPUTime = (span: string) => {
  const dot = span.indexOf(".");
  return dot > 0 && !span.includes("=") && (cpuTimeFields[span.slice(0, dot)]?.(span.slice(dot + 1)) ?? false);
};

export type BenchEvent = Record<string, unknown> & { event: string };

export function readBenchReport(path: string): { events: BenchEvent[]; unreadable: number } {
  const events: BenchEvent[] = [];
  let unreadable = 0;
  for (const line of (readText(path) ?? "").split("\n")) {
    if (!line) continue;
    try {
      const event = JSON.parse(line);
      if (typeof event?.event === "string") events.push(event);
      else ++unreadable;
    } catch {
      ++unreadable;
    }
  }
  return { events, unreadable };
}

// ---------------------------------------------------------------------------------------------------------------------
// Spans

// One process's spans. An occurrence event gives its count and, for each field, the sum of its numbers, the number of
// events where a boolean holds, or the number of events with each value of a string, named <event>.<field>=<value>. A
// flush event gives the fields of its last line. Keys are left out; the installation bound joins by them (below).
export function processSpans(events: BenchEvent[]): Map<string, number> {
  const spans = new Map<string, number>();
  const add = (name: string, value: number) => spans.set(name, (spans.get(name) ?? 0) + value);
  const fields = (event: BenchEvent) => Object.entries(event).filter(([field]) => !["event", "pid", "vm", "key"].includes(field));
  const lastFlush = new Map<string, BenchEvent>();
  for (const event of events) {
    if (flushEvents.has(event.event)) {
      lastFlush.set(event.event, event);
      continue;
    }
    add(`${event.event}.count`, 1);
    for (const [field, value] of fields(event)) {
      if (typeof value === "number") add(`${event.event}.${field}`, value);
      else if (typeof value === "boolean") add(`${event.event}.${field}`, value ? 1 : 0);
      else if (typeof value === "string") add(`${event.event}.${field}=${value}`, 1);
    }
  }
  for (const [name, event] of lastFlush) {
    for (const [field, value] of fields(event)) {
      if (typeof value === "number" || typeof value === "boolean") spans.set(`${name}.${field}`, Number(value));
    }
  }
  return spans;
}

export interface SpanSummary {
  min: number;
  median: number;
  n: number; // the copies that measured the span
  values: (number | null)[]; // each copy's value, null where it measured none
}

export function median(values: number[]): number {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = sorted.length >> 1;
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

export function summarize(values: (number | null)[]): SpanSummary | null {
  const measured = values.filter((value): value is number => value !== null);
  return measured.length ? { min: Math.min(...measured), median: median(measured), n: measured.length, values } : null;
}

// A run's spans by copy, from each copy's spans, or null for a copy that wrote no bench report or was dropped. An
// occurrence event's span is 0 in a copy whose report lacks the event, since that copy's count and sums are 0; a flush
// event's span is measured only in the copies that wrote it.
export function spansByCopy(copies: (Map<string, number> | null)[]): Map<string, (number | null)[]> {
  const names = [...new Set(copies.flatMap(spans => (spans ? [...spans.keys()] : [])))].sort();
  const byCopy = new Map<string, (number | null)[]>();
  for (const name of names) {
    const flush = flushEvents.has(name.slice(0, name.indexOf(".")));
    byCopy.set(name, copies.map(spans => (spans ? (spans.get(name) ?? (flush ? null : 0)) : null)));
  }
  return byCopy;
}

// ---------------------------------------------------------------------------------------------------------------------
// The installation bound

// A body's import cost is its import request's counted total plus its install's total, joined by key with a
// request that came before the install in its process; its native cost is its compile event's compilation plus
// finalization in processes whose artifact holds no body and whose role records nothing. An import takes the place of a
// body's first compilation, so a process gives a body at most one sample per side: on the import side its first
// install, on the native side its first compile. A sample whose compile or install generated per-VM support does not
// count, and a body left with no counting sample on a side is set aside.

interface ImportSample {
  nanoseconds: number; // request + install
  request: number; // the request event's counted total
  install: number; // the install event's total, the install function's thread CPU time
  action: string; // the request's action, import
  imageCodeSize: number; // the image's linked size
  allocation: number; // the installed code's executable allocation, the install event's codeSize
  fixups: number; // the image's fixup count
  steps: Record<string, number>; // the install's steps that ran, wall time that explains the total
}

interface NativeSample {
  nanoseconds: number; // compile + finalize
  compile: number;
  finalize: number;
  allocation: number; // the native code's executable allocation, the compile event's codeSize
}

export interface BodyTally {
  key: string;
  importSamples: ImportSample[];
  nativeSamples: NativeSample[];
  installSupportGenerated: number;
  installWithoutRequest: number; // installs that no import request of the key preceded in their process
  compileSupportGenerated: number;
  supportGeneratedNanoseconds: number[]; // the native compiles that generated support, which only the whole native cost counts
  requestedWithoutInstall: number; // import-side processes that imported the body and never installed it
  compiledOnImportSide: number; // import-side processes that compiled the body natively
  firstImageCodeSize: number | null; // the first install's image.codeSize, the size of a body without a counting import sample
  imageCodeSizes: Set<number>; // the image.codeSize of each process's install
}

const numberField = (event: BenchEvent, field: string) => (typeof event[field] === "number" ? (event[field] as number) : 0);

function tallyOf(tallies: Map<string, BodyTally>, key: string): BodyTally {
  let tally = tallies.get(key);
  if (!tally) {
    tally = {
      key,
      importSamples: [],
      nativeSamples: [],
      installSupportGenerated: 0,
      installWithoutRequest: 0,
      compileSupportGenerated: 0,
      supportGeneratedNanoseconds: [],
      requestedWithoutInstall: 0,
      compiledOnImportSide: 0,
      firstImageCodeSize: null,
      imageCodeSizes: new Set(),
    };
    tallies.set(key, tally);
  }
  return tally;
}

function firstByKey(events: BenchEvent[], test: (event: BenchEvent) => boolean): Map<string, BenchEvent> {
  const first = new Map<string, BenchEvent>();
  for (const event of events) {
    if (typeof event.key === "string" && test(event) && !first.has(event.key)) first.set(event.key, event);
  }
  return first;
}

// One import-side process. Its events come in the order the process wrote them, so an install joins only a request
// written before it.
export function addImportSide(tallies: Map<string, BodyTally>, events: BenchEvent[]) {
  const requests = new Map<string, BenchEvent>(); // each key's first import request so far
  const installed = new Set<string>();
  for (const event of events) {
    const key = event.key;
    if (typeof key !== "string") continue;
    if (event.event === "request" && event.action === "import") {
      if (!requests.has(key)) requests.set(key, event);
      continue;
    }
    if (event.event !== "install" || installed.has(key)) continue;
    installed.add(key);
    const tally = tallyOf(tallies, key);
    const imageCodeSize = numberField(event, "image.codeSize");
    tally.firstImageCodeSize ??= imageCodeSize;
    tally.imageCodeSizes.add(imageCodeSize);
    if (event.supportGenerated === true) {
      ++tally.installSupportGenerated;
      continue;
    }
    const request = requests.get(key);
    if (!request) {
      ++tally.installWithoutRequest;
      continue;
    }
    const steps: Record<string, number> = {};
    for (const [field, value] of Object.entries(event)) {
      if (field.startsWith("steps.") && typeof value === "number") steps[field.slice("steps.".length)] = value;
    }
    const requestNanoseconds = numberField(request, "counted");
    const installNanoseconds = numberField(event, "total");
    tally.importSamples.push({
      nanoseconds: requestNanoseconds + installNanoseconds,
      request: requestNanoseconds,
      install: installNanoseconds,
      action: String(request.action),
      imageCodeSize,
      allocation: numberField(event, "codeSize"),
      fixups: numberField(event, "image.fixupCount"),
      steps,
    });
  }
  for (const key of requests.keys()) {
    if (!installed.has(key)) ++tallyOf(tallies, key).requestedWithoutInstall;
  }
  for (const key of firstByKey(events, event => event.event === "compile").keys()) ++tallyOf(tallies, key).compiledOnImportSide;
}

// One native-side process, a Consumer whose artifact holds no body, since a Producer's compiles carry the cost of
// recording. Returns its compiles without a key, whose UCB the parent-key registry did not hold, and the compilation and
// finalization time of every compile it made, the process's whole native cost.
export function addNativeSide(tallies: Map<string, BodyTally>, events: BenchEvent[]): { keyless: number; nanoseconds: number } {
  const time = (compile: BenchEvent) => numberField(compile, "compileNanoseconds") + numberField(compile, "finalizeNanoseconds");
  for (const [key, compile] of firstByKey(events, event => event.event === "compile")) {
    const tally = tallyOf(tallies, key);
    if (compile.supportGenerated === true) {
      ++tally.compileSupportGenerated;
      tally.supportGeneratedNanoseconds.push(time(compile));
      continue;
    }
    tally.nativeSamples.push({ nanoseconds: time(compile), compile: numberField(compile, "compileNanoseconds"), finalize: numberField(compile, "finalizeNanoseconds"), allocation: numberField(compile, "codeSize") });
  }
  const compiles = events.filter(event => event.event === "compile");
  return { keyless: compiles.filter(compile => compile.key === null).length, nanoseconds: compiles.reduce((sum, compile) => sum + time(compile), 0) };
}

export type BodyStatus = "joined" | "set aside" | "native only" | "import only";

// Why a side has no counting sample: per-VM support generated in each of its samples, or installs that no import request
// preceded, as an attach's.
interface SetAside {
  side: "import" | "native";
  reason: "supportGenerated" | "installWithoutImportRequest";
  samples: number;
}

interface SideValue<Sample> {
  samples: number; // the counting ones
  min: number | null;
  median: number | null;
  minimum: Sample | null; // the sample that gave the minimum
}

export interface BoundBody {
  key: string;
  status: BodyStatus;
  // The installed image's linked size, which installationBoundSmallestBody reads: the size in the sample that gave the
  // import minimum, or the first install's for a body without a counting import sample.
  imageCodeSize: number | null;
  imageCodeSizeVaries: boolean; // the installs of different copies reported different sizes
  installedAllocation: number | null; // the executable allocation in the sample that gave the import minimum
  nativeAllocation: number | null; // the one in the sample that gave the native minimum, which may differ
  ratio: number | null; // the import minimum over the native minimum
  // The body's part of the whole native cost: its native minimum, or for a body set aside on the native side, the least
  // time of its compiles that generated support.
  nativeCost: number | null;
  import: SideValue<ImportSample> & { supportGenerated: number; withoutImportRequest: number; requestedWithoutInstall: number; compiledNatively: number };
  native: SideValue<NativeSample> & { supportGenerated: number };
  setAside: SetAside[];
}

function sideValue<Sample extends { nanoseconds: number }>(samples: Sample[]): SideValue<Sample> {
  if (!samples.length) return { samples: 0, min: null, median: null, minimum: null };
  const minimum = samples.reduce((least, sample) => (sample.nanoseconds < least.nanoseconds ? sample : least));
  return { samples: samples.length, min: minimum.nanoseconds, median: median(samples.map(sample => sample.nanoseconds)), minimum };
}

export function boundBody(tally: BodyTally): BoundBody {
  const imported = sideValue(tally.importSamples);
  const compiled = sideValue(tally.nativeSamples);
  const setAside: SetAside[] = [];
  if (!imported.samples) {
    if (tally.installSupportGenerated) setAside.push({ side: "import", reason: "supportGenerated", samples: tally.installSupportGenerated });
    if (tally.installWithoutRequest) setAside.push({ side: "import", reason: "installWithoutImportRequest", samples: tally.installWithoutRequest });
  }
  if (!compiled.samples && tally.compileSupportGenerated) setAside.push({ side: "native", reason: "supportGenerated", samples: tally.compileSupportGenerated });
  const status: BodyStatus = imported.samples && compiled.samples ? "joined" : setAside.length ? "set aside" : compiled.samples ? "native only" : "import only";
  return {
    key: tally.key,
    status,
    imageCodeSize: imported.minimum ? imported.minimum.imageCodeSize : tally.firstImageCodeSize,
    imageCodeSizeVaries: tally.imageCodeSizes.size > 1,
    installedAllocation: imported.minimum?.allocation ?? null,
    nativeAllocation: compiled.minimum?.allocation ?? null,
    ratio: status === "joined" && compiled.min ? imported.min! / compiled.min : null,
    nativeCost: compiled.min ?? (tally.supportGeneratedNanoseconds.length ? Math.min(...tally.supportGeneratedNanoseconds) : null),
    import: {
      ...imported,
      supportGenerated: tally.installSupportGenerated,
      withoutImportRequest: tally.installWithoutRequest,
      requestedWithoutInstall: tally.requestedWithoutInstall,
      compiledNatively: tally.compiledOnImportSide,
    },
    native: { ...compiled, supportGenerated: tally.compileSupportGenerated },
    setAside,
  };
}

export interface BoundCoverage {
  script: string | null; // null for every script together
  smallestBody: number;
  covered: number; // the joined bodies of at least S
  nativeShare: number | null; // their native cost over the whole native cost
  excludedShare: number | null; // the native cost of the bodies smaller than S, joined or set aside, over the whole
  p50: number | null; // nearest-rank percentiles of the covered bodies' ratios
  p99: number | null;
  max: number | null;
}

const percentile = (sorted: number[], p: number) => (sorted.length ? sorted[Math.ceil((p / 100) * sorted.length) - 1] : null);

// The joined bodies a smallest body S covers, those whose image size is at least S, and their ratios, against a whole
// native cost the caller chooses. The bench mode passes every compile its native copies made, set-aside and keyless
// ones included, as the bound's rule reads a workload's whole native compile time; the bound's own bench passes its
// joined bodies' native cost.
export function boundCoverage(bodies: BoundBody[], wholeNativeCost: number, smallestBody: number, script: string | null): BoundCoverage {
  const covered = bodies.filter(body => body.status === "joined" && body.imageCodeSize! >= smallestBody);
  const excluded = bodies.filter(body => body.imageCodeSize !== null && body.imageCodeSize < smallestBody);
  const nativeCost = (list: BoundBody[]) => list.reduce((total, body) => total + (body.nativeCost ?? 0), 0);
  const ratios = covered.map(body => body.ratio!).sort((a, b) => a - b);
  return {
    script,
    smallestBody,
    covered: covered.length,
    nativeShare: wholeNativeCost ? nativeCost(covered) / wholeNativeCost : null,
    excludedShare: wholeNativeCost ? nativeCost(excluded) / wholeNativeCost : null,
    p50: percentile(ratios, 50),
    p99: percentile(ratios, 99),
    max: ratios.length ? ratios[ratios.length - 1] : null,
  };
}

// ---------------------------------------------------------------------------------------------------------------------
// Text both benches print

export const loadText = (load: number[] | null) => (load ? load.map(value => value.toFixed(2)).join(" ") : "unknown");
export const ratioText = (ratio: number | null) => (ratio === null ? "n/a" : ratio.toFixed(3));
export const percentText = (share: number | null) => (share === null ? "n/a" : `${(100 * share).toFixed(1)}%`);

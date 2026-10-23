// Draws jsc_jit_tiers.png, the README's chart of JavaScriptCore's tiers, in the layout and style of
// the matplotlib figure it replaced (200 dpi, DejaVu Sans). Run it from anywhere:
//
//   bun docs/images/jsc_jit_tiers.ts
//
// Bun installs @resvg/resvg-js on first use. DEJAVU_FONT_DIR overrides where the fonts are looked up.

import { Resvg } from "@resvg/resvg-js@2.6.2";
import { existsSync } from "node:fs";
import { join } from "node:path";

type Tier = {
  name: string;
  speed: number;
  color: string;
  // Calls a function without loops makes before it runs in this tier: the earliest and the latest.
  reach: [number, number];
  calls: string;
  speedNote?: string;
  // Annotations sit up and to the right of the tier's marker unless this says left.
  labelSide?: "left";
  compile: number;
  compileLabel: string;
  compileNote?: string;
};

const tiers: Tier[] = [
  { name: "LLInt", speed: 1, color: "#9aa5b1", reach: [0, 0], calls: "0 calls", compile: 0, compileLabel: "none" },
  { name: "Baseline", speed: 2, color: "#3e8ed0", reach: [35, 35], calls: "~35 calls", labelSide: "left", compile: 1, compileLabel: "1×" },
  { name: "DFG", speed: 4, color: "#f2a541", reach: [100, 600], calls: "100–600 calls", compile: 4, compileLabel: "4×" },
  { name: "FTL", speed: 6, color: "#e4572e", reach: [5000, 12000], calls: "5k–12k calls", speedNote: "  (1.5× DFG)", compile: 24, compileLabel: "24×", compileNote: "6× DFG" },
];

const ink = "#1f2933";
const muted = "#52606d";
const spine = "#9aa5b1";
const grid = "#e4e7eb";

// Pixels per typographic point at the figure's 200 dpi, and the font sizes in points.
const pt = 200 / 72;
const fontSize = { title: 15, panel: 13, axis: 11, annotation: 10.5, small: 9.5 };
const width = 2623;
const height = 1183;

const speedAxes = { left: 124.5, right: 1627.5, top: 120.5, bottom: 1074.5 };
const costAxes = { left: 2026.5, right: 2603.5, top: 120.5, bottom: 1074.5 };

// Matplotlib's symlog scale with linthresh 1 and linscale 0.35: [0, 1] is linear and spans 0.389 of
// a decade, and each decade above 1 spans the same width.
const linearDecades = 0.35 / (1 - 1 / 10);
const decades = (calls: number) =>
  Math.sign(calls) * (Math.abs(calls) <= 1 ? Math.abs(calls) * linearDecades : linearDecades + Math.log10(Math.abs(calls)));
const callsDomain = [-0.15, 20000] as const;
const callsX = (calls: number) =>
  speedAxes.left +
  ((decades(calls) - decades(callsDomain[0])) / (decades(callsDomain[1]) - decades(callsDomain[0]))) *
    (speedAxes.right - speedAxes.left);
const speedTop = 7.4;
const speedY = (speed: number) => speedAxes.bottom - (speed / speedTop) * (speedAxes.bottom - speedAxes.top);

const costRight = 29;
const costX = (cost: number) => costAxes.left + (cost / costRight) * (costAxes.right - costAxes.left);
// Rows count down from the top; bars are 0.6 rows tall, with matplotlib's 5% margins around them.
const barHeight = 0.6;
const rowMargin = barHeight / 2 + 0.05 * (tiers.length - 1 + barHeight);
const rowY = (row: number) =>
  costAxes.top + ((row + rowMargin) / (tiers.length - 1 + 2 * rowMargin)) * (costAxes.bottom - costAxes.top);
const rowHeight = (costAxes.bottom - costAxes.top) / (tiers.length - 1 + 2 * rowMargin);

const parts: string[] = [];

const attributes = (values: Record<string, string | number | undefined>) =>
  Object.entries(values)
    .filter(([, value]) => value !== undefined)
    .map(([name, value]) => `${name}="${typeof value === "number" ? +value.toFixed(2) : value}"`)
    .join(" ");

const element = (name: string, values: Record<string, string | number | undefined>) =>
  parts.push(`<${name} ${attributes(values)}/>`);

const line = (x1: number, y1: number, x2: number, y2: number, stroke: string, extra: Record<string, string | number> = {}) =>
  element("line", { x1, y1, x2, y2, stroke, "stroke-width": 0.8 * pt, ...extra });

type TextStyle = { size: number; bold?: boolean; color?: string; anchor?: "start" | "middle" | "end"; rotate?: number };

function text(x: number, y: number, content: string, style: TextStyle) {
  const escaped = content.replace(/&/g, "&amp;").replace(/</g, "&lt;");
  parts.push(
    `<text ${attributes({
      x: style.rotate ? 0 : x,
      y: style.rotate ? 0 : y,
      transform: style.rotate ? `translate(${x} ${y}) rotate(${style.rotate})` : undefined,
      "font-size": style.size * pt,
      "font-weight": style.bold ? "bold" : undefined,
      fill: style.color ?? ink,
      "text-anchor": style.anchor ?? "start",
    })} xml:space="preserve">${escaped}</text>`,
  );
}

element("rect", { width, height, fill: "#ffffff" });

// Execution speed: the grid first, so the translucent fills tint it.
for (let speed = 1; speed <= 7; speed++) line(speedAxes.left, speedY(speed), speedAxes.right, speedY(speed), grid);
const callTicks: [number, string][] = [[0, "0"], [1, "1"], [10, "10"], [100, "100"], [1000, "1k"], [10000, "10k"]];
for (const [calls] of callTicks)
  line(callsX(calls), speedAxes.top, callsX(calls), speedAxes.bottom, grid, { "stroke-dasharray": `${0.8 * pt} ${1.32 * pt}` });

// Under the line, each tier's color covers the calls where a function has certainly reached it, from
// the latest call that reaches it to the latest call that reaches the next tier. Above that, a lighter
// band spans the calls where the function may already run in the tier.
tiers.forEach((tier, i) => {
  const end = i + 1 < tiers.length ? tiers[i + 1].reach[1] : callsDomain[1];
  const x = callsX(tier.reach[1]);
  element("rect", { x, y: speedY(tier.speed), width: callsX(end) - x, height: speedAxes.bottom - speedY(tier.speed), fill: tier.color, "fill-opacity": 0.18 });
  if (i > 0 && tier.reach[0] < tier.reach[1]) {
    const bandX = callsX(tier.reach[0]);
    element("rect", { x: bandX, y: speedY(tier.speed), width: x - bandX, height: speedY(tiers[i - 1].speed) - speedY(tier.speed), fill: tier.color, "fill-opacity": 0.09 });
  }
});

line(speedAxes.left, speedAxes.top, speedAxes.left, speedAxes.bottom, spine);
line(speedAxes.left, speedAxes.bottom, speedAxes.right, speedAxes.bottom, spine);

// The solid line steps up at the latest call that reaches each tier, the dashed one at the earliest.
const speedLine = { fill: "none", stroke: ink, "stroke-width": 2.2 * pt, "stroke-linejoin": "round" };
let latest = `M ${callsX(0)} ${speedY(tiers[0].speed)}`;
for (const tier of tiers.slice(1)) latest += ` H ${callsX(tier.reach[1])} V ${speedY(tier.speed)}`;
element("path", { d: `${latest} H ${callsX(callsDomain[1])}`, ...speedLine });
tiers.forEach((tier, i) => {
  if (i === 0 || tier.reach[0] === tier.reach[1]) return;
  const d = `M ${callsX(tier.reach[0])} ${speedY(tiers[i - 1].speed)} V ${speedY(tier.speed)} H ${callsX(tier.reach[1])}`;
  element("path", { d, ...speedLine, "stroke-dasharray": `${3.7 * 2.2 * pt} ${1.6 * 2.2 * pt}` });
});

for (const tier of tiers) {
  const x = callsX(tier.reach[0]);
  const y = speedY(tier.speed);
  element("circle", { cx: x, cy: y, r: 5 * pt, fill: tier.color, stroke: "#ffffff", "stroke-width": 2 * pt });
  const left = tier.labelSide === "left";
  const labelX = left ? x - 8 * pt : x + 8 * pt;
  const anchor = left ? "end" : "start";
  const lines = [`${tier.speed}× speed${tier.speedNote ?? ""}`, tier.calls];
  text(labelX, y - 38 * pt, tier.name, { size: fontSize.annotation, bold: true, anchor });
  lines.forEach((content, n) => text(labelX, y - (25.2 - 12.8 * n) * pt, content, { size: fontSize.annotation, anchor }));
}

// Ticks are 3.5 points long, and their labels sit 3.5 points past them.
const tick = 3.5 * pt;
for (let speed = 0; speed <= 7; speed++) {
  line(speedAxes.left - tick, speedY(speed), speedAxes.left, speedY(speed), muted);
  text(speedAxes.left - 2 * tick, speedY(speed) + 4.45 * pt, `${speed}×`, { size: fontSize.axis, color: muted, anchor: "end" });
}
for (const [calls, label] of callTicks) {
  line(callsX(calls), speedAxes.bottom, callsX(calls), speedAxes.bottom + tick, muted);
  text(callsX(calls), speedAxes.bottom + 15.1 * pt, label, { size: fontSize.axis, color: muted, anchor: "middle" });
}
text((speedAxes.left + speedAxes.right) / 2, speedAxes.bottom + 29.2 * pt, "Calls to reach tier (log scale)", { size: fontSize.axis, anchor: "middle" });
text(speedAxes.left - 30 * pt, (speedAxes.top + speedAxes.bottom) / 2, "Relative speed (vs. LLInt)", { size: fontSize.axis, anchor: "middle", rotate: -90 });
text(speedAxes.left, speedAxes.top - 5.4 * pt, "Execution speed", { size: fontSize.panel, bold: true });

// Compile cost.
for (let cost = 5; cost <= 25; cost += 5) line(costX(cost), costAxes.top, costX(cost), costAxes.bottom, grid);
tiers.forEach((tier, row) => {
  if (tier.compile > 0)
    element("rect", { x: costX(0), y: rowY(row) - (barHeight / 2) * rowHeight, width: costX(tier.compile) - costX(0), height: barHeight * rowHeight, fill: tier.color });
});
line(costAxes.left, costAxes.top, costAxes.left, costAxes.bottom, spine);
line(costAxes.left, costAxes.bottom, costAxes.right, costAxes.bottom, spine);
const costGap = costX(0.6) - costX(0);
tiers.forEach((tier, row) => {
  text(costAxes.left - tick, rowY(row) + 4.45 * pt, tier.name, { size: fontSize.axis, anchor: "end" });
  text(costX(tier.compile) + costGap, rowY(row) + 3.1 * pt, tier.compileLabel, { size: fontSize.annotation, bold: true });
  if (tier.compileNote)
    text(costX(tier.compile) - costGap, rowY(row) + 2.5 * pt, tier.compileNote, { size: fontSize.small, color: "#ffffff", anchor: "end" });
});
for (let cost = 0; cost <= 25; cost += 5) {
  line(costX(cost), costAxes.bottom, costX(cost), costAxes.bottom + tick, muted);
  text(costX(cost), costAxes.bottom + 15.1 * pt, `${cost}`, { size: fontSize.axis, color: muted, anchor: "middle" });
}
text((costAxes.left + costAxes.right) / 2, costAxes.bottom + 29.2 * pt, "Compile time (× Baseline)", { size: fontSize.axis, anchor: "middle" });
text(costAxes.left, costAxes.top - 5.4 * pt, "Compile cost", { size: fontSize.panel, bold: true });

text(width / 2, 18.9 * pt, "JSC JIT Tiers", { size: fontSize.title, anchor: "middle" });

const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}" font-family="DejaVu Sans">${parts.join("")}</svg>`;

const fontDirectory = [process.env.DEJAVU_FONT_DIR, "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/dejavu", "/usr/share/fonts/TTF"]
  .filter((directory): directory is string => !!directory)
  .find(directory => existsSync(join(directory, "DejaVuSans.ttf")) && existsSync(join(directory, "DejaVuSans-Bold.ttf")));
if (!fontDirectory) throw new Error("DejaVu Sans not found: set DEJAVU_FONT_DIR to the directory with DejaVuSans.ttf and DejaVuSans-Bold.ttf");

const png = new Resvg(svg, {
  font: {
    loadSystemFonts: false,
    fontFiles: [join(fontDirectory, "DejaVuSans.ttf"), join(fontDirectory, "DejaVuSans-Bold.ttf")],
    defaultFontFamily: "DejaVu Sans",
  },
}).render().asPng();
await Bun.write(join(import.meta.dir, "jsc_jit_tiers.png"), png);

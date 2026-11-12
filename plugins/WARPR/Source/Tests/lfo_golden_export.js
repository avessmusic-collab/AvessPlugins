// LFO golden-vector exporter (plan D8: "golden vectors exported from the production UI JS").
//
//   node plugins/WARPR/Source/Tests/lfo_golden_export.js
//
// Loads Source/ui/public/index.html, extracts the REAL presetFn() and the bend-anchor code of lfoRender() (verbatim, by
// text), evaluates them on a fixed grid and prints C++ rows for PhaseD8Tests.cpp:
//
//     { "SINE", 1, 0.0925, 0.49512...},
//
// Paste the output between the BEGIN GOLDEN / END GOLDEN markers of kJsGolden[] in PhaseD8Tests.cpp.  Rows are in the UI's y
// coordinate (y = 0 is the top); the test compares  table.eval(t)  with  1 - y.
// The preset parameter sets MUST match the test: bend set index -> goldenBendSets() in PhaseD8Tests.cpp, S&H table and
// DRIFT phases -> goldenShape().

const fs = require("fs");
const path = require("path");

const html = fs.readFileSync(path.join(__dirname, "..", "ui", "public", "index.html"), "utf8");

const a = html.indexOf("function stepsFn");
const b = html.indexOf("const led = $");
if (a < 0 || b < 0) throw new Error("presetFn block not found in index.html");

const TAU = Math.PI * 2;
const presetFn = new Function("TAU", html.slice(a, b) + "\nreturn presetFn;")(TAU);

const m = html.match(/const fp = \[\.\.\.sh\.bends\][\s\S]*?return fn\(t\) \+ a0\.dy\*\(1 - uu\) \+ a1\.dy\*uu;\s*\};/);
if (!m) throw new Error("bend anchor code not found in index.html");
const makeYs = new Function("fn", "sh", m[0] + "\nreturn ys;");

const presets = ["SINE", "TRIANGLE", "SAW UP", "SAW DOWN", "SQUARE", "PUMP", "STEPS 4", "STEPS 8", "STEPS 16", "SWING", "S&H NOISE", "DRIFT"];
const bendSets = [
  [],
  [{ x: 0.5, y: 0.2 }],
  [{ x: 0.3, y: 0.8 }, { x: 0.7, y: 0.15 }],
  [{ x: 0.25, y: 0.9 }, { x: 0.4, y: 0.1 }, { x: 0.9, y: 0.5 }]
];

const rows = [];
for (const preset of presets) {
  bendSets.forEach((bends, bi) => {
    const sh = { preset, points: [], bends, table: null, drift: null };
    if (preset === "S&H NOISE") sh.table = [0.31, 0.74, 0.12, 0.55, 0.9, 0.2, 0.66, 0.43];
    if (preset === "DRIFT") sh.drift = { a: 1.3, b: 4.1 };
    const fn = presetFn(sh);
    const ys = makeYs(fn, sh);
    const ts = [];
    for (let k = 0; k < 4001; k += 97) ts.push((k + 0.37) / 4001);
    for (let k = 0; k < 16; k++) ts.push(k / 16);
    for (const t of ts) rows.push(`        { "${preset}", ${bi}, ${t.toPrecision(17)}, ${ys(t).toPrecision(17)} },`);
  });
}
console.log(rows.join("\n"));

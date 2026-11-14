// Checks every locale in src/i18n/locales against en.json: missing and extra keys, and
// interpolations ({{name}}) or tags (<code>) a translation lost. Plural variants (_one, _few,
// ...) are free: each language has its own.
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";

const root = join(import.meta.dirname, "..", "src", "i18n", "locales");
const PLURAL = /_(zero|one|two|few|many|other)$/;
const flat = (d, prefix = "") =>
  Object.entries(d).reduce((out, [k, v]) => Object.assign(out, v && typeof v === "object" ? flat(v, `${prefix}${k}.`) : { [`${prefix}${k}`]: v }), {});
const marks = (s) => new Set(String(s).match(/{{\w+}}|<\/?\w+>/g) ?? []);
const read = (name) => flat(JSON.parse(readFileSync(join(root, name), "utf8")));

const ref = read("en.json");
const base = new Set(Object.keys(ref).map((k) => k.replace(PLURAL, "")));
// the reference text of a key or of its plural group (for marks)
const refText = (k) => ref[k] ?? ref[`${k.replace(PLURAL, "")}_other`] ?? ref[k.replace(PLURAL, "")] ?? "";
let failed = false;
for (const name of readdirSync(root).filter((f) => f.endsWith(".json") && f !== "en.json").sort()) {
  const loc = read(name);
  const keys = Object.keys(loc);
  const have = new Set(keys.map((k) => k.replace(PLURAL, "")));
  const missing = [...base].filter((k) => !have.has(k));
  const extra = keys.filter((k) => !base.has(k.replace(PLURAL, "")));
  const lost = keys.filter((k) => [...marks(refText(k))].some((m) => !marks(loc[k]).has(m)));
  const ok = !missing.length && !extra.length && !lost.length;
  console.log(`${name.padEnd(12)} ${String(keys.length).padStart(4)} keys  ${ok ? "ok" : "PROBLEMS"}`);
  for (const [label, items] of [["missing", missing], ["extra", extra], ["lost interpolation or tag", lost]]) {
    if (!items.length) continue;
    failed = true;
    console.log(`   ${label}: ${items.slice(0, 12).join(", ")}${items.length > 12 ? " …" : ""}`);
  }
}
process.exit(failed ? 1 : 0);

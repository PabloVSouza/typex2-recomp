/** Controller layouts: what each game's cabinet buttons are called, its default layout (as
 *  runtime/src/padmap.c has it), and X2_PAD strings to and from a full button -> action map.
 *  X2_PAD keys: a b c d = KOF's A-D (cabinet buttons 1, 4, 2, 5), 1-6 = cabinet buttons 1-6,
 *  t = Test, v = Service. Button and action names are shown as they are; hints are i18n keys. */
import type { ParseKeys } from "i18next";

export type PadButton = "south" | "east" | "west" | "north" | "l1" | "r1" | "l2" | "r2" | "l3" | "r3";

export const PAD_BUTTONS: { id: PadButton; label: string }[] = [
  { id: "south", label: "A / ✕" },
  { id: "east", label: "B / ○" },
  { id: "west", label: "X / □" },
  { id: "north", label: "Y / △" },
  { id: "l1", label: "LB / L1" },
  { id: "r1", label: "RB / R1" },
  { id: "l2", label: "LT / L2" },
  { id: "r2", label: "RT / R2" },
  { id: "l3", label: "L3" },
  { id: "r3", label: "R3" },
];

/** A cabinet action: the X2_PAD keys it presses, its name (shown as it is, or an i18n key) and a hint. */
export interface Action { keys: string; name?: string; nameKey?: ParseKeys; hint?: ParseKeys }

type Family = "kof" | "sf4" | "bb";

const SYSTEM: Action[] = [
  { keys: "t", nameKey: "controls.test", hint: "controls.hints.test" },
  { keys: "v", nameKey: "controls.service", hint: "controls.hints.service" },
];

const ACTIONS: Record<Family, Action[]> = {
  kof: [
    { keys: "a", name: "A", hint: "controls.hints.lightPunch" },
    { keys: "b", name: "B", hint: "controls.hints.lightKick" },
    { keys: "c", name: "C", hint: "controls.hints.heavyPunch" },
    { keys: "d", name: "D", hint: "controls.hints.heavyKick" },
    { keys: "ab", name: "A + B", hint: "controls.hints.evade" },
    { keys: "cd", name: "C + D", hint: "controls.hints.blowback" },
    { keys: "bc", name: "B + C" },
    { keys: "abc", name: "A + B + C" },
    { keys: "bcd", name: "B + C + D" },
    ...SYSTEM,
  ],
  sf4: [
    { keys: "1", name: "LP", hint: "controls.hints.lightPunch" },
    { keys: "2", name: "MP", hint: "controls.hints.mediumPunch" },
    { keys: "3", name: "HP", hint: "controls.hints.heavyPunch" },
    { keys: "4", name: "LK", hint: "controls.hints.lightKick" },
    { keys: "5", name: "MK", hint: "controls.hints.mediumKick" },
    { keys: "6", name: "HK", hint: "controls.hints.heavyKick" },
    { keys: "123", name: "LP + MP + HP", hint: "controls.hints.punches" },
    { keys: "456", name: "LK + MK + HK", hint: "controls.hints.kicks" },
    ...SYSTEM,
  ],
  bb: [
    { keys: "1", name: "A", hint: "controls.hints.weak" },
    { keys: "2", name: "B", hint: "controls.hints.medium" },
    { keys: "3", name: "C", hint: "controls.hints.heavy" },
    { keys: "4", name: "D", hint: "controls.hints.drive" },
    { keys: "12", name: "A + B", hint: "controls.hints.barrier" },
    { keys: "23", name: "B + C", hint: "controls.hints.throw" },
    { keys: "123", name: "A + B + C" },
    { keys: "1234", name: "A + B + C + D" },
    ...SYSTEM,
  ],
};

/** The games' defaults (padmap.c x2_padmap): FinalBurn Neo's Neo Geo pad for KOF, the panel's
 *  rows on Y X R1 / B A R2 for SF4 and BlazBlue. */
const DEFAULTS: Record<Family, string> = {
  kof: "west=a,south=b,north=c,east=d,r2=ab,r1=cd,l2=abc,l1=bcd,l3=-,r3=-",
  sf4: "west=1,north=2,r1=3,south=4,east=5,r2=6,l1=123,l2=456,l3=-,r3=-",
  bb: "west=1,north=2,r1=3,south=4,east=23,r2=12,l1=123,l2=1234,l3=-,r3=-",
};

function family(game: string): Family {
  if (game === "ssf4ae") return "sf4";
  if (game === "bbcs" || game === "bbct") return "bb";
  return "kof";
}

export const actionsFor = (game: string) => ACTIONS[family(game)];

/** The same keys in one spelling: sorted, no duplicates; "" for none. */
const norm = (keys: string) => [...new Set(keys.toLowerCase().replace(/[^a-d1-6tv]/g, ""))].sort().join("");

export type Layout = Record<PadButton, string>;

function parse(pad: string, into: Layout): Layout {
  for (const part of pad.split(/[,\s]+/)) {
    const [button, keys] = part.split("=");
    if (button && keys !== undefined && button.toLowerCase() in into) into[button.toLowerCase() as PadButton] = norm(keys);
  }
  return into;
}

/** The layout an X2_PAD string gives: the game's default with the string's buttons on top, as
 *  the runtime reads it ("" is the default). */
export function layoutOf(game: string, pad: string): Layout {
  const base = parse(DEFAULTS[family(game)], Object.fromEntries(PAD_BUTTONS.map((b) => [b.id, ""])) as Layout);
  return parse(pad, base);
}

/** Every button spelled out, so what the launcher shows is exactly what the game gets. */
export function padOf(layout: Layout): string {
  return PAD_BUTTONS.map((b) => `${b.id}=${layout[b.id] || "-"}`).join(",");
}

export const isDefault = (game: string, layout: Layout) => padOf(layout) === padOf(layoutOf(game, ""));

/** The buttons pressing an action, in PAD_BUTTONS order. */
export const buttonsFor = (layout: Layout, keys: string) => PAD_BUTTONS.filter((b) => layout[b.id] === norm(keys)).map((b) => b.id);

export const buttonLabel = (id: PadButton) => PAD_BUTTONS.find((b) => b.id === id)?.label ?? id;

/** The layout with `button` pressing `keys` (taken off whatever it pressed before). */
export const assign = (layout: Layout, button: PadButton, keys: string): Layout => ({ ...layout, [button]: norm(keys) });

/** The layout with an action back to the game's default buttons: those that press it by default
 *  do again, and others pressing it now go back to their own default. */
export function resetAction(game: string, layout: Layout, keys: string): Layout {
  const def = layoutOf(game, "");
  const k = norm(keys);
  const next = { ...layout };
  for (const b of PAD_BUTTONS) {
    if (def[b.id] === k || next[b.id] === k) next[b.id] = def[b.id];
  }
  return next;
}

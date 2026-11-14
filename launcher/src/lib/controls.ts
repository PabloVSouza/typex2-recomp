/** Controller layouts: what each game's cabinet buttons are called, its default layout (as
 *  runtime/src/padmap.c has it), and X2_PAD strings to and from a full button -> action map.
 *  X2_PAD keys: a b c d = KOF's A-D (cabinet buttons 1, 4, 2, 5), 1-6 = cabinet buttons 1-6,
 *  t = Test, v = Service. */

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

/** A cabinet action: the X2_PAD keys it presses, and its name. */
export interface Action { keys: string; label: string; hint?: string }

type Family = "kof" | "sf4" | "bb";

const SYSTEM: Action[] = [
  { keys: "t", label: "Test", hint: "the operator menu" },
  { keys: "v", label: "Service", hint: "a free credit" },
];

const ACTIONS: Record<Family, Action[]> = {
  kof: [
    { keys: "a", label: "A", hint: "light punch" },
    { keys: "b", label: "B", hint: "light kick" },
    { keys: "c", label: "C", hint: "heavy punch" },
    { keys: "d", label: "D", hint: "heavy kick" },
    { keys: "ab", label: "A + B", hint: "evade" },
    { keys: "cd", label: "C + D", hint: "blowback" },
    { keys: "bc", label: "B + C" },
    { keys: "abc", label: "A + B + C" },
    { keys: "bcd", label: "B + C + D" },
    ...SYSTEM,
  ],
  sf4: [
    { keys: "1", label: "LP", hint: "light punch" },
    { keys: "2", label: "MP", hint: "medium punch" },
    { keys: "3", label: "HP", hint: "heavy punch" },
    { keys: "4", label: "LK", hint: "light kick" },
    { keys: "5", label: "MK", hint: "medium kick" },
    { keys: "6", label: "HK", hint: "heavy kick" },
    { keys: "123", label: "3 punches" },
    { keys: "456", label: "3 kicks" },
    ...SYSTEM,
  ],
  bb: [
    { keys: "1", label: "A", hint: "weak" },
    { keys: "2", label: "B", hint: "medium" },
    { keys: "3", label: "C", hint: "heavy" },
    { keys: "4", label: "D", hint: "drive" },
    { keys: "12", label: "A + B", hint: "barrier" },
    { keys: "23", label: "B + C", hint: "throw" },
    { keys: "123", label: "A + B + C" },
    { keys: "1234", label: "A + B + C + D" },
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

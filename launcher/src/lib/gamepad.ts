/** Android: the layout editor reads a controller button from the page (the Gamepad API's standard
 *  mapping), as there is no x2-player there. Resolves with the X2_PAD name of the first button
 *  newly pressed, or null after `timeout` ms. */
const STANDARD: Record<number, string> = { 0: "south", 1: "east", 2: "west", 3: "north", 4: "l1", 5: "r1", 6: "l2", 7: "r2", 10: "l3", 11: "r3" };
/** A pad Android doesn't know (an arcade stick) has no standard mapping: its face buttons and
 *  shoulders keep the standard numbers, but L2 / R2 are triggers on axes 4 / 5, resting at -1 and
 *  at +1 when pressed. */
const TRIGGER_AXES: [number, string][] = [[4, "l2"], [5, "r2"]];

/** Controller navigation (useControllerNav) stands aside while a button is being read here. */
export const padNav = { paused: false };

/** The Android app also reports every controller button and trigger itself (MainActivity calls
 *  window.x2PadButton with its X2_PAD name): the WebView's Gamepad API shows some pads only after
 *  certain presses. */
const nativePresses = new Set<(name: string) => void>();
(window as unknown as { x2PadButton?: (name: string, down: boolean) => void }).x2PadButton = (name, down) => {
  if (down) for (const f of [...nativePresses]) f(name);
};

/** The X2_PAD names of the buttons down on every connected pad. */
function pressedNames(): string[] {
  const out: string[] = [];
  for (const p of navigator.getGamepads()) {
    if (!p) continue;
    p.buttons.forEach((b, i) => b.pressed && i in STANDARD && out.push(STANDARD[i]));
    if (p.mapping !== "standard") for (const [a, name] of TRIGGER_AXES) if ((p.axes[a] ?? -1) > 0.5) out.push(name);
  }
  return out;
}

export function readGamepadButton(timeout = 10000): Promise<string | null> {
  const held = new Set(pressedNames()); // buttons already down (the one that pressed Assign) don't count
  const end = performance.now() + timeout;
  padNav.paused = true;
  return new Promise((done) => {
    let over = false;
    const finish = (v: string | null) => {
      if (over) return;
      over = true;
      nativePresses.delete(onNative);
      setTimeout(() => (padNav.paused = false), 400); // the button's release is not a press
      done(v);
    };
    const onNative = (name: string) => finish(name);
    nativePresses.add(onNative);
    const poll = () => {
      if (over) return;
      const now = pressedNames();
      const fresh = now.find((n) => !held.has(n));
      if (fresh !== undefined) return finish(fresh);
      for (const n of [...held]) if (!now.includes(n)) held.delete(n);
      if (performance.now() > end) return finish(null);
      requestAnimationFrame(poll);
    };
    poll();
  });
}

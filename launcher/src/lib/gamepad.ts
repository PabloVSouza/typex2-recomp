/** Android: the layout editor reads a controller button from the page (the Gamepad API's standard
 *  mapping), as there is no x2-player there. Resolves with the X2_PAD name of the first button
 *  newly pressed, or null after `timeout` ms. */
const STANDARD: Record<number, string> = { 0: "south", 1: "east", 2: "west", 3: "north", 4: "l1", 5: "r1", 6: "l2", 7: "r2", 10: "l3", 11: "r3" };

/** Controller navigation (useControllerNav) stands aside while a button is being read here. */
export const padNav = { paused: false };

export function readGamepadButton(timeout = 10000): Promise<string | null> {
  const pressed = () =>
    navigator.getGamepads().flatMap((p) => (p ? p.buttons.map((b, i) => (b.pressed ? i : -1)).filter((i) => i in STANDARD) : []));
  const held = new Set(pressed()); // buttons already down (the one that pressed Assign) don't count
  const end = performance.now() + timeout;
  padNav.paused = true;
  const finish = (v: string | null, done: (v: string | null) => void) => {
    setTimeout(() => (padNav.paused = false), 400); // the button's release is not a press
    done(v);
  };
  return new Promise((done) => {
    const poll = () => {
      const now = pressed();
      const fresh = now.find((i) => !held.has(i));
      if (fresh !== undefined) return finish(STANDARD[fresh], done);
      for (const i of [...held]) if (!now.includes(i)) held.delete(i);
      if (performance.now() > end) return finish(null, done);
      requestAnimationFrame(poll);
    };
    poll();
  });
}

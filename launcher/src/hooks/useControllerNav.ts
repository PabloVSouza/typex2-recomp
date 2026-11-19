import { useEffect } from "react";
import { moveFocus, padNav, type Dir } from "@/lib";
import { useUi } from "@/stores";

const KEYS: Record<string, Dir> = { ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" };
/** standard mapping: d-pad 12-15, A 0, B 1 */
const PAD_DIRS: [number, Dir][] = [[12, "up"], [13, "down"], [14, "left"], [15, "right"]];
const FIRST_REPEAT = 350, REPEAT = 110, DEAD = 0.5;
/** Android also turns a pad's d-pad and A into arrow / Enter keys: those right after the same
 *  move by the Gamepad API are dropped */
const DUP_MS = 120;

function typing(el: Element | null) {
  if (!(el instanceof HTMLElement)) return false;
  if (el.isContentEditable || el instanceof HTMLTextAreaElement) return true;
  return el instanceof HTMLInputElement && !["checkbox", "radio", "button", "submit", "range"].includes(el.type);
}

/** Navigating the launcher with a controller (and the arrow keys): the d-pad or the left stick
 *  moves the focus to the nearest control that way, A presses it (a game card: plays it), X opens
 *  a card's page, B closes a menu or dialog
 *  (Escape) or goes back to the library. The highlight (data-pad) shows once a controller's button
 *  is pressed, until the next touch or click. */
export function useControllerNav() {
  useEffect(() => {
    const root = document.documentElement;
    const usingPad = () => root.setAttribute("data-pad", "");
    const notPad = () => root.removeAttribute("data-pad");
    let lastPad = { dir: "" as string, at: 0 }, lastKey = { dir: "" as string, at: 0 };
    // the first move starts in the page (Android focuses the sidebar's first item by itself)
    let started = false;
    const move = (dir: Dir) => {
      moveFocus(dir, !started);
      started = true;
    };
    const reset = () => {
      notPad();
      started = false;
    };

    // B, and Android's Back (MainActivity calls window.x2Back): false when there is nothing to
    // close or leave, so the app may; a pad's B arrives both ways, the second within 250 ms is dropped
    let lastBack = 0;
    const back = (): boolean => {
      const now = performance.now();
      if (now - lastBack < 250) return true;
      lastBack = now;
      const el = (document.activeElement as HTMLElement | null) ?? document.body;
      const open = document.querySelector("[role=dialog], [role=alertdialog], [role=listbox], [role=menu]");
      if (open) {
        el.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", code: "Escape", bubbles: true }));
        return true;
      }
      const { page, go } = useUi.getState();
      if (page === "library") return false;
      go("library");
      return true;
    };
    (window as unknown as { x2Back?: () => boolean }).x2Back = back;

    // A: the focused control's "padactivate" (a game card launches its game), else a click; X:
    // "padsecondary" (a card opens its page)
    const activate = (type: "padactivate" | "padsecondary") => {
      const el = document.activeElement as HTMLElement | null;
      if (!el || el === document.body) return;
      const ev = new CustomEvent(type, { cancelable: true });
      if (el.dispatchEvent(ev) && type === "padactivate") el.click();
    };

    const onKey = (e: KeyboardEvent) => {
      const dir = KEYS[e.key];
      if (dir) {
        if (typing(document.activeElement) && (dir === "left" || dir === "right")) return;
        if (document.querySelector("[role=listbox], [role=menu]")) return; // a select's own keys
        e.preventDefault();
        if (lastPad.dir === dir && performance.now() - lastPad.at < DUP_MS) return;
        lastKey = { dir, at: performance.now() };
        if (!e.code) usingPad(); // Android's d-pad keys (a controller) carry no code; arrow keys do
        move(dir);
      } else if (e.key === "Enter" && lastPad.dir === "a" && performance.now() - lastPad.at < DUP_MS) {
        e.preventDefault(); // the pad's A, already pressed
      }
    };

    // Gamepad API: polled while a pad is connected
    let raf = 0;
    const held = new Map<string, number>(); // control -> next repeat time
    const poll = () => {
      raf = requestAnimationFrame(poll);
      if (padNav.paused) return;
      const now = performance.now();
      const pads = navigator.getGamepads?.() ?? [];
      const on = new Set<string>();
      for (const p of pads) {
        if (!p) continue;
        for (const [i, d] of PAD_DIRS) if (p.buttons[i]?.pressed) on.add(d);
        const [x = 0, y = 0] = p.axes;
        if (y < -DEAD) on.add("up");
        if (y > DEAD) on.add("down");
        if (x < -DEAD) on.add("left");
        if (x > DEAD) on.add("right");
        if (p.buttons[0]?.pressed) on.add("a");
        if (p.buttons[2]?.pressed) on.add("x");
        if (p.buttons[1]?.pressed) on.add("b");
      }
      for (const c of [...held.keys()]) if (!on.has(c)) held.delete(c);
      for (const c of on) {
        const next = held.get(c);
        if (next !== undefined && (c === "a" || c === "b" || c === "x" || now < next)) continue;
        held.set(c, now + (next === undefined ? FIRST_REPEAT : REPEAT));
        usingPad();
        lastPad = { dir: c, at: now };
        if (c === "a") activate("padactivate");
        else if (c === "x") activate("padsecondary");
        else if (c === "b") back();
        else if (!(lastKey.dir === c && now - lastKey.at < DUP_MS)) move(c as Dir); // its key came first
      }
    };
    const start = () => {
      if (!raf) raf = requestAnimationFrame(poll);
    };
    if ((navigator.getGamepads?.() ?? []).some(Boolean)) start();
    window.addEventListener("gamepadconnected", start);
    window.addEventListener("keydown", onKey, true);
    window.addEventListener("pointerdown", reset, true);
    return () => {
      delete (window as unknown as { x2Back?: () => boolean }).x2Back;
      cancelAnimationFrame(raf);
      window.removeEventListener("gamepadconnected", start);
      window.removeEventListener("keydown", onKey, true);
      window.removeEventListener("pointerdown", reset, true);
    };
  }, []);
}

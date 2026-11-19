import { useEffect } from "react";
import { moveFocus, padNav, type Dir } from "@/lib";
import { useUi } from "@/stores";

const KEYS: Record<string, Dir> = { ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" };
/** standard mapping: d-pad 12-15, A 0, B 1, X 2, Y 3 */
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
 *  a card's page, Y shows or hides the sidebar, B closes a menu or dialog
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
    const narrow = () => window.matchMedia("(max-width: 767px)").matches; // the rail: always shown
    const later = (f: () => void) => requestAnimationFrame(() => requestAnimationFrame(f)); // after React
    const focusPage = (el?: HTMLElement | null) => {
      if (!el?.isConnected) return void moveFocus("down", true);
      el.focus({ preventScroll: true });
      el.scrollIntoView({ block: "center", inline: "nearest", behavior: "smooth" });
    };

    // A controller puts the launcher in full-screen mode, once per session (the menu button by the
    // logo brings the menu back for good). The menu then shows over the page for a moment (menuPeek):
    // left from the page's leftmost control or Y opens it and moves the focus there; going back to
    // the page (right, B, Y, or choosing a page) hides it again.
    let seen = false;
    const controllerSeen = () => {
      if (seen) return;
      seen = true;
      const ui = useUi.getState();
      if (!ui.menuHidden && !narrow()) {
        const inMenu = document.activeElement?.closest("aside");
        ui.setMenuHidden(true);
        if (inMenu) focusPage(null);
      }
    };
    let returnTo: HTMLElement | null = null; // where the focus was in the page
    const openMenu = () => {
      const ui = useUi.getState();
      if (!ui.menuHidden || ui.menuPeek || narrow()) return;
      const cur = document.activeElement as HTMLElement | null;
      returnTo = cur?.closest("main") ? cur : null;
      ui.setMenuPeek(true);
      // its current item, once it is reachable (no longer inert)
      later(() => (document.querySelector<HTMLElement>("aside nav [aria-current=page]") ?? document.querySelector<HTMLElement>("aside nav button"))?.focus({ preventScroll: true }));
    };
    const closeMenu = (refocus: boolean) => {
      const ui = useUi.getState();
      if (!ui.menuPeek) return false;
      ui.setMenuPeek(false);
      if (refocus) focusPage(returnTo);
      returnTo = null;
      return true;
    };
    // Y: shows or hides the menu
    const toggleMenu = () => {
      const ui = useUi.getState();
      if (narrow() || closeMenu(true)) return;
      if (ui.menuHidden) return openMenu();
      const inMenu = document.activeElement?.closest("aside");
      ui.setMenuHidden(true);
      if (inMenu) focusPage(null);
    };

    const move = (dir: Dir) => {
      const peek = useUi.getState().menuPeek;
      if (peek && dir === "right" && document.activeElement?.closest("aside")) return void closeMenu(true);
      const next = moveFocus(dir, !started);
      started = true;
      if (next) {
        if (peek && next.closest("main")) closeMenu(false);
        return;
      }
      const main = document.activeElement?.closest("main");
      if (!main) return;
      if (dir === "left") openMenu();
      // no control further that way: the page itself scrolls, to its text below or above
      else if (dir === "down" || dir === "up") main.scrollBy({ top: (dir === "down" ? 0.6 : -0.6) * main.clientHeight, behavior: "smooth" });
    };
    const reset = (e: Event) => {
      notPad();
      started = false;
      // a touch or click in the page hides the menu shown over it
      if (!(e.target instanceof Element && e.target.closest("aside, [data-brandbar]"))) closeMenu(false);
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
      if (closeMenu(true)) return true;
      const { page, go } = useUi.getState();
      if (page === "library") return false;
      go("library");
      return true;
    };
    (window as unknown as { x2Back?: () => boolean }).x2Back = back;

    // A: the focused control's "padactivate" (a game card launches its game), else a click; X:
    // "padsecondary" (a card opens its page). A page chosen in the menu shown over the page hides
    // it, the focus going to the new page.
    const activate = (type: "padactivate" | "padsecondary") => {
      const el = document.activeElement as HTMLElement | null;
      if (!el || el === document.body) return;
      const ev = new CustomEvent(type, { cancelable: true });
      if (!el.dispatchEvent(ev) || type !== "padactivate") return;
      const before = useUi.getState();
      el.click();
      if (!before.menuPeek || !el.closest("aside")) return;
      const now = useUi.getState();
      if (now.page === before.page && now.gameId === before.gameId) return void closeMenu(true);
      returnTo = null;
      closeMenu(false);
      later(() => focusPage(null));
    };

    const onKey = (e: KeyboardEvent) => {
      const dir = KEYS[e.key];
      if (dir) {
        if (typing(document.activeElement) && (dir === "left" || dir === "right")) return;
        if (document.querySelector("[role=listbox], [role=menu]")) return; // a select's own keys
        e.preventDefault();
        if (lastPad.dir === dir && performance.now() - lastPad.at < DUP_MS) return;
        lastKey = { dir, at: performance.now() };
        usingPad(); // the arrow keys too: one highlight style, never the outline
        if (!e.code) controllerSeen(); // Android's d-pad keys (a controller) carry no code; arrow keys do
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
        if (p.buttons[3]?.pressed) on.add("y");
      }
      for (const c of [...held.keys()]) if (!on.has(c)) held.delete(c);
      for (const c of on) {
        const next = held.get(c);
        if (next !== undefined && (c === "a" || c === "b" || c === "x" || c === "y" || now < next)) continue;
        held.set(c, now + (next === undefined ? FIRST_REPEAT : REPEAT));
        usingPad();
        controllerSeen();
        lastPad = { dir: c, at: now };
        if (c === "a") activate("padactivate");
        else if (c === "x") activate("padsecondary");
        else if (c === "b") back();
        else if (c === "y") toggleMenu();
        else if (!(lastKey.dir === c && now - lastKey.at < DUP_MS)) move(c as Dir); // its key came first
      }
    };
    const start = () => {
      controllerSeen();
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

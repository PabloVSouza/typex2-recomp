/** Moving the focus with a controller or the arrow keys: the nearest control in a direction. */
export type Dir = "up" | "down" | "left" | "right";

const SELECTOR = [
  "button:not([disabled])",
  "a[href]",
  "input:not([disabled]):not([type=hidden])",
  "select:not([disabled])",
  "textarea:not([disabled])",
  "[role=switch]",
  "[role=combobox]",
  "[role=option]",
  "[role=menuitem]",
  "[tabindex]:not([tabindex='-1'])",
]
  .map((s) => `${s}:not([tabindex="-1"])`) // taken out of the focus order (a game card's inner buttons)
  .join(",");

function visible(el: HTMLElement): boolean {
  if (el.closest("[aria-hidden=true], [inert]")) return false;
  const r = el.getBoundingClientRect();
  return r.width > 0 && r.height > 0 && getComputedStyle(el).visibility !== "hidden";
}

/** The controls the focus can reach: inside an open dialog or menu when there is one. */
export function focusables(): HTMLElement[] {
  const layers = document.querySelectorAll<HTMLElement>("[role=dialog], [role=alertdialog], [role=listbox], [role=menu]");
  const scope = layers.length ? layers[layers.length - 1] : document.body;
  return Array.from(scope.querySelectorAll<HTMLElement>(SELECTOR)).filter(visible);
}

/** The control to focus from `from` towards `dir` (null: none that way). */
export function nearest(from: HTMLElement, dir: Dir, all: HTMLElement[]): HTMLElement | null {
  const a = from.getBoundingClientRect();
  const ax = a.left + a.width / 2, ay = a.top + a.height / 2;
  let best: HTMLElement | null = null;
  let bestScore = Infinity;
  for (const el of all) {
    if (el === from || el.contains(from) || from.contains(el)) continue;
    const b = el.getBoundingClientRect();
    const bx = b.left + b.width / 2, by = b.top + b.height / 2;
    // how far along the direction (must be ahead) and how far off its axis
    let along: number, across: number;
    switch (dir) {
      case "down": along = b.top - a.bottom; across = Math.abs(bx - ax); break;
      case "up": along = a.top - b.bottom; across = Math.abs(bx - ax); break;
      case "right": along = b.left - a.right; across = Math.abs(by - ay); break;
      default: along = a.left - b.right; across = Math.abs(by - ay); break;
    }
    if (along < -4) continue;
    const score = Math.max(along, 0) + across * 2;
    if (score < bestScore) {
      bestScore = score;
      best = el;
    }
  }
  return best;
}

/** The part of the window a control is in (the page, the sidebar, ...). */
const region = (el: Element) => el.closest("main, aside, header, [role=dialog], [role=alertdialog], [role=listbox], [role=menu]");

/** Moves the focus; to the page's first control when nothing is focused or when `fresh` (the first
 *  move: Android focuses the sidebar's first item itself). Up and down stay in the current part of
 *  the window (the page's next control may be off screen, farther than the sidebar's); left and
 *  right cross between them. Returns the focused control, null when there is none that way. */
export function moveFocus(dir: Dir, fresh = false): HTMLElement | null {
  const all = focusables();
  const cur = document.activeElement as HTMLElement | null;
  // the page, not the sidebar: its first game card, else its first control
  const start = all.find((el) => el.hasAttribute("data-card")) ?? all.find((el) => el.closest("main")) ?? all[0] ?? null;
  let next: HTMLElement | null = start;
  if (!fresh && cur && cur !== document.body && all.includes(cur)) {
    const here = region(cur);
    // the menu button by the logo (BrandBar) is above both
    const ok = (el: HTMLElement) => region(el) === here || !!el.closest("[data-brandbar]");
    next = nearest(cur, dir, here && (dir === "up" || dir === "down") ? all.filter(ok) : all);
  }
  if (next) {
    next.focus({ preventScroll: true });
    // kept in the middle of the page, so what comes next is in view
    next.scrollIntoView({ block: "center", inline: "nearest", behavior: "smooth" });
  }
  return next;
}

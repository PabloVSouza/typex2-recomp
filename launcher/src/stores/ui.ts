import { create } from "zustand";

export type Page = "library" | "game" | "export" | "tools" | "log" | "about";

interface Ui {
  page: Page;
  gameId: string | null;
  /** full-screen mode: the sidebar hidden, a button by the logo brings it back (remembered) */
  menuHidden: boolean;
  /** in full-screen mode, the sidebar shown over the page for a moment (a controller opened it) */
  menuPeek: boolean;
  go: (page: Page, gameId?: string) => void;
  setMenuHidden: (hidden: boolean) => void;
  setMenuPeek: (peek: boolean) => void;
}

// "#tools", "#game:kof13": open on that page (screenshots of the browser preview)
const [hashPage, hashGame] = (typeof location !== "undefined" ? location.hash.slice(1) : "").split(":");
const PAGES: Page[] = ["library", "game", "export", "tools", "log", "about"];
const MENU_KEY = "x2.menuHidden";
const stored = (() => {
  try {
    return localStorage.getItem(MENU_KEY) === "1";
  } catch {
    return false;
  }
})();

/** The page shown and the game it is about. */
export const useUi = create<Ui>((set) => ({
  page: PAGES.includes(hashPage as Page) ? (hashPage as Page) : "library",
  gameId: hashGame ?? null,
  menuHidden: stored,
  menuPeek: false,
  go: (page, gameId) => set((s) => ({ page, gameId: gameId ?? s.gameId })),
  setMenuHidden: (menuHidden) => {
    try {
      localStorage.setItem(MENU_KEY, menuHidden ? "1" : "0");
    } catch {
      /* a convenience: not kept without storage */
    }
    set({ menuHidden, menuPeek: false });
  },
  setMenuPeek: (menuPeek) => set({ menuPeek }),
}));

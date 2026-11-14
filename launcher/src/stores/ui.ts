import { create } from "zustand";

export type Page = "library" | "game" | "export" | "tools" | "log" | "about";

interface Ui {
  page: Page;
  gameId: string | null;
  go: (page: Page, gameId?: string) => void;
}

// "#tools", "#game:kof13": open on that page (screenshots of the browser preview)
const [hashPage, hashGame] = (typeof location !== "undefined" ? location.hash.slice(1) : "").split(":");
const PAGES: Page[] = ["library", "game", "export", "tools", "log", "about"];

/** The page shown and the game it is about. */
export const useUi = create<Ui>((set) => ({
  page: PAGES.includes(hashPage as Page) ? (hashPage as Page) : "library",
  gameId: hashGame ?? null,
  go: (page, gameId) => set((s) => ({ page, gameId: gameId ?? s.gameId })),
}));

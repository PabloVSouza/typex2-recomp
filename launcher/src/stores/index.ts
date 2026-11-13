import { create } from "zustand";
import type { JobEvent } from "@/lib/types";

export type Page = "library" | "game" | "export" | "tools" | "log" | "about";

interface Ui {
  page: Page;
  gameId: string | null;
  go: (page: Page, gameId?: string) => void;
}

// "#tools", "#game:kof13": open on that page (screenshots of the browser preview)
const [hashPage, hashGame] = (typeof location !== "undefined" ? location.hash.slice(1) : "").split(":");
const PAGES: Page[] = ["library", "game", "export", "tools", "log", "about"];

export const useUi = create<Ui>((set) => ({
  page: PAGES.includes(hashPage as Page) ? (hashPage as Page) : "library",
  gameId: hashGame ?? null,
  go: (page, gameId) => set((s) => ({ page, gameId: gameId ?? s.gameId })),
}));

const MAX_LINES = 4000;
const append = (lines: string[], line: string) => (lines.length >= MAX_LINES ? [...lines.slice(-MAX_LINES + 1), line] : [...lines, line]);

interface Activity {
  /** The latest event of the running (or last) job. */
  job: JobEvent | null;
  buildLog: string[];
  gameLog: string[];
  onJob: (e: JobEvent) => void;
  onBuildLine: (line: string) => void;
  onGameLine: (line: string) => void;
  clear: (which: "build" | "game") => void;
}

export const useActivity = create<Activity>((set) => ({
  job: null,
  buildLog: [],
  gameLog: [],
  onJob: (job) => set({ job }),
  onBuildLine: (line) => set((s) => ({ buildLog: append(s.buildLog, line) })),
  onGameLine: (line) => set((s) => ({ gameLog: append(s.gameLog, line) })),
  clear: (which) => set(which === "build" ? { buildLog: [] } : { gameLog: [] }),
}));

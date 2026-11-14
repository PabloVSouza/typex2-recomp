import { create } from "zustand";
import type { JobEvent } from "@/lib";

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

/** What the backend reports while it works: the job's progress and the build and game output. */
export const useActivity = create<Activity>((set) => ({
  job: null,
  buildLog: [],
  gameLog: [],
  onJob: (job) => set({ job }),
  onBuildLine: (line) => set((s) => ({ buildLog: append(s.buildLog, line) })),
  onGameLine: (line) => set((s) => ({ gameLog: append(s.gameLog, line) })),
  clear: (which) => set(which === "build" ? { buildLog: [] } : { gameLog: [] }),
}));

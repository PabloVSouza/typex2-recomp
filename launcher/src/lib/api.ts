import type { AppStateData, GameSettings } from "@/lib/types";
import { mockInvoke } from "@/lib/mockApi";

/** In the app: the Rust commands (src-tauri/src/lib.rs). In a plain browser (npm run dev):
 *  the mock, so the pages can be worked on without the app. */
export const inTauri = typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;

async function call<T>(cmd: string, args?: Record<string, unknown>): Promise<T> {
  if (!inTauri) return mockInvoke(cmd, args) as Promise<T>;
  const { invoke } = await import("@tauri-apps/api/core");
  return invoke<T>(cmd, args);
}

export const api = {
  loadState: () => call<AppStateData>("load_state"),
  addGame: (path: string) => call<{ id: string; title: string }>("add_game", { path }),
  removeGame: (id: string, deleteBuilds: boolean) => call<void>("remove_game", { id, deleteBuilds }),
  saveGameSettings: (id: string, settings: GameSettings) => call<void>("save_game_settings", { id, settings }),
  saveBuildSettings: (ndk: string, jobs: number) => call<void>("save_build_settings", { ndk, jobs }),
  buildLibrary: (id: string) => call<void>("build_library", { id }),
  exportTarget: (target: string, ids: string[]) => call<void>("export", { target, ids }),
  installTools: (ids: string[]) => call<void>("install_tools", { ids }),
  removeTools: () => call<void>("remove_tools"),
  cancelJob: () => call<void>("cancel_job"),
  deleteBuilds: (id: string) => call<void>("delete_builds", { id }),
  launch: (id: string) => call<void>("launch", { id }),
  stop: () => call<void>("stop"),
  /** The next controller button pressed (X2_PAD name), or null when cancelled. */
  readPad: () => call<string | null>("read_pad"),
};

export async function pickFolder(title: string): Promise<string | null> {
  if (!inTauri) return "/Games/The King of Fighters XIII";
  const { open } = await import("@tauri-apps/plugin-dialog");
  const r = await open({ directory: true, multiple: false, title });
  return typeof r === "string" ? r : null;
}

export async function reveal(path: string) {
  if (!inTauri) return;
  const { revealItemInDir } = await import("@tauri-apps/plugin-opener");
  await revealItemInDir(path);
}

export async function listen<T>(event: string, handler: (payload: T) => void): Promise<() => void> {
  if (!inTauri) return () => {};
  const { listen } = await import("@tauri-apps/api/event");
  return listen<T>(event, (e) => handler(e.payload));
}

/** A web page in the system's browser. */
export async function openUrl(url: string) {
  if (!inTauri) return void window.open(url, "_blank");
  const { openUrl } = await import("@tauri-apps/plugin-opener");
  await openUrl(url);
}

export interface Layout { label: string; pad: string }
export interface GameInfo { id: string; title: string; board: string; year: number; publisher: string; layouts: Layout[] }
export interface GameSettings { fullscreen: boolean; fps: boolean; pad: string; scale: number; vsync: boolean; log_level: number; env: string }
export interface LibraryRecord { built: number; seconds: number; size: number; functions: number }
export interface LibraryStatus { state: "none" | "ready" | "stale"; built: LibraryRecord | null }
export interface GameEntry { id: string; path: string; settings: GameSettings; info: GameInfo | null; found: boolean; library: LibraryStatus }
export interface Target { id: string; name: string; description: string; needs: string[] }
export interface ExportRecord { target: string; games: string[]; files: string[]; built: number }
export interface Tool { id: string; name: string; path: string | null; source: "kit" | "downloaded" | "system" | null; for_what: string; download: string | null; hint: string }
export interface Package { id: string; name: string; version: string; size: number; for_what: string }
export interface Kit { source: string; x2recomp: string | null; player: string | null; kind: "checkout" | "bundled" }
export interface AppStateData {
  version: string; os: string; arch: string; data: string; kit: Kit | null; can_play: boolean;
  catalog: GameInfo[]; games: GameEntry[]; ndk: string; jobs: number; job: string | null; running: string | null;
  targets: Target[]; exports: ExportRecord[]; tools: Tool[]; packages: Package[]; tools_size: number;
}
/** A "job" event: one step of a build, download or export. */
export interface JobEvent {
  job: string;
  step: "hash" | "generate" | "compile" | "link" | "configure" | "build" | "download" | "unpack" | "done" | "error" | "cancelled";
  game?: string; tool?: string; done?: number; total?: number; message?: string; result?: unknown;
}

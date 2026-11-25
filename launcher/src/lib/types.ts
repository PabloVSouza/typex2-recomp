export interface Layout { id: string; label: string; pad: string }
export interface GameInfo { id: string; title: string; board: string; year: number; publisher: string; layouts: Layout[] }
/** touch: the on-screen touch pad, 0 automatic, 1 always, 2 never; touch_buttons: its buttons (X2_TOUCH_BUTTONS:
 *  "" the game's layout, "neogeo" A B C D in a row, or "1=a,2=c,..." slots 1-4 top row, 5-8 bottom) */
export interface GameSettings { fullscreen: boolean; fps: boolean; pad: string; scale: number; vsync: boolean; log_level: number; touch: number; touch_buttons: string; env: string }
export interface LibraryRecord { built: number; seconds: number; size: number; functions: number }
export interface LibraryStatus { state: "none" | "ready" | "stale"; built: LibraryRecord | null }
/** info: null for a game not supported yet (title: its folder's name) */
export interface GameEntry { id: string; path: string; title: string; supported: boolean; settings: GameSettings; info: GameInfo | null; found: boolean; library: LibraryStatus }
/** The Android app (android.rs): all-files access, the ROMs folder scanned, where packs arrive. */
export interface AndroidInfo { all_files: boolean; roms: string; incoming: string | null }
export interface Target { id: string; name: string; description: string; needs: string[] }
export interface ExportRecord { target: string; games: string[]; files: string[]; built: number }
export interface Tool { id: string; name: string; path: string | null; source: "kit" | "downloaded" | "system" | null; for_what: string; download: string | null; hint: string }
export interface Package { id: string; name: string; version: string; size: number; for_what: string }
export interface Kit { source: string; x2recomp: string | null; player: string | null; kind: "checkout" | "bundled" }
export interface AppStateData {
  version: string; os: string; arch: string; data: string; kit: Kit | null; can_play: boolean; android: AndroidInfo | null;
  catalog: GameInfo[]; games: GameEntry[]; ndk: string; jobs: number; language: string; check_updates: boolean; job: string | null; running: string | null;
  targets: Target[]; exports: ExportRecord[]; tools: Tool[]; packages: Package[]; tools_size: number;
}
/** A "job" event: one step of a build, download or export. */
export interface JobEvent {
  job: string;
  step: "hash" | "generate" | "compile" | "link" | "configure" | "build" | "download" | "unpack" | "done" | "error" | "cancelled";
  game?: string; tool?: string; done?: number; total?: number; message?: string; result?: unknown;
}
/** A launcher release on GitHub (updates.rs). */
export interface ReleaseInfo { version: string; tag: string; newer: boolean; notes: string }
/** An Android device adb sees (device.rs); state "device" when usable. */
export interface Device { serial: string; model: string; state: string }
export interface DeviceList { adb: boolean; devices: Device[] }

import type { AppStateData, GameInfo, GameSettings } from "@/lib/types";

/** Browser preview only: a library with a few games in different states. */
type Layouts = GameInfo["layouts"];
const DEFAULT = { id: "default", label: "Game default", pad: "" };
const kof: Layouts = [DEFAULT, { id: "stick-kof", label: "8-button stick (L1 / L2 rows): A C over B D", pad: "l1=a,west=c,north=cd,r1=bcd,l2=b,south=d,east=ab,r2=abc" }];
const sf4: Layouts = [DEFAULT, { id: "stick-sf4", label: "8-button stick (L1 / L2 rows)", pad: "l1=1,west=2,north=3,r1=123,l2=4,south=5,east=6,r2=456" }];
const bb: Layouts = [DEFAULT, { id: "stick-bb", label: "8-button stick, Type B panel", pad: "l1=1,west=2,north=3,r1=123,l2=4,south=23,east=12,r2=1234" }];
const sse: Layouts = [DEFAULT, { id: "stick-sse", label: "8-button stick: A B C over R", pad: "l1=1,west=2,north=3,r1=123,l2=4,south=23,east=12,r2=1234" }];
const gok: Layouts = [DEFAULT, { id: "stick-gok", label: "8-button stick (L1 / L2 rows): punches over kicks", pad: "l1=1,west=2,north=25,r1=245,l2=4,south=5,east=14,r2=124" }];
const catalog: GameInfo[] = [
  { id: "kof12", title: "The King of Fighters XII", board: "Type X2", year: 2009, publisher: "SNK Playmore", layouts: kof },
  { id: "kof13", title: "The King of Fighters XIII", board: "Type X2", year: 2010, publisher: "SNK Playmore", layouts: kof },
  { id: "kof13cl", title: "The King of Fighters XIII Climax", board: "Type X2", year: 2012, publisher: "SNK Playmore", layouts: kof },
  { id: "ssf4ae", title: "Super Street Fighter IV Arcade Edition", board: "Type X2", year: 2010, publisher: "Capcom", layouts: sf4 },
  { id: "bbcs", title: "BlazBlue: Continuum Shift", board: "Type X2", year: 2009, publisher: "Arc System Works", layouts: bb },
  { id: "bbct", title: "BlazBlue: Calamity Trigger", board: "Type X2", year: 2008, publisher: "Arc System Works", layouts: bb },
  { id: "kof98um", title: "The King of Fighters '98 Ultimate Match", board: "Type X", year: 2008, publisher: "SNK Playmore", layouts: kof },
  { id: "kofmira", title: "The King of Fighters: Maximum Impact Regulation A", board: "Type X2", year: 2007, publisher: "SNK Playmore", layouts: kof },
  { id: "sse", title: "Samurai Shodown: Edge of Destiny", board: "Type X2", year: 2008, publisher: "SNK Playmore", layouts: sse },
  { id: "goketsuji", title: "Gouketsuji Ichizoku Matsuri Senzo Kuyou", board: "Type X2", year: 2012, publisher: "Atlus", layouts: gok },
  { id: "kof2002um", title: "The King of Fighters 2002 Unlimited Match", board: "Type X2", year: 2011, publisher: "SNK Playmore", layouts: kof },
];
const settings = { fullscreen: false, fps: false, pad: "", scale: 0, vsync: false, log_level: 0, touch: 0, touch_buttons: "", env: "" };
const entry = (id: string, state: "none" | "ready" | "stale") => ({
  id, path: `/Games/${catalog.find((g) => g.id === id)!.title}`, title: catalog.find((g) => g.id === id)!.title, supported: true, settings, info: catalog.find((g) => g.id === id)!, found: true,
  library: { state, built: state === "none" ? null : { built: 1763000000, seconds: 74, size: 11228104, functions: 10967 } },
});
// a game the recompiler does not support yet: listed under its folder's name
const other = { id: "other-00a3c000-0045e2f1", path: "/Games/Raiden IV.x2", title: "Raiden IV", supported: false, settings, info: null, found: true, library: { state: "none" as const, built: null } };
// ?android in the preview: the Android app's pages
const android = typeof window !== "undefined" && new URLSearchParams(window.location.search).has("android");
const state: AppStateData = {
  version: "0.1.0", os: android ? "android" : "macos", arch: "aarch64", data: "~/Library/Application Support/io.github.typex2-recomp.launcher",
  kit: { source: "~/typex2-recomp", x2recomp: "build/recompiler/x2recomp", player: "build/x2-player", kind: "checkout" },
  can_play: true, catalog, games: [entry("kof13", "ready"), entry("ssf4ae", "none"), entry("bbcs", android ? "none" : "stale"), entry("kof98um", "ready"), other],
  android: android ? { all_files: false, roms: "/storage/emulated/0/ROMs/arcade", incoming: "/storage/emulated/0/TypeX2/packs" } : null,
  ndk: "", jobs: 0, language: "", check_updates: true, job: null, running: null,
  targets: [
    { id: "android-retroarch", name: "RetroArch core for Android", description: "typex2_libretro_android.so with the chosen games. Copy it to RetroArch/downloads/ on the device, then Load Core → Install or Restore a Core.", needs: ["x2recomp", "cmake", "ninja", "ndk"] },
    { id: "android-packs", name: "Game packs for the Android app", description: "One pack per chosen game for the Type X2 app.", needs: ["x2recomp", "cmake", "ninja", "ndk"] },
    { id: "host-retroarch", name: "RetroArch core for this computer", description: "The libretro core for RetroArch on this computer, with the chosen games.", needs: ["x2recomp", "cmake", "ninja", "cc"] },
  ],
  exports: [{ target: "android-retroarch", games: ["kof13", "kof98um"], files: ["exports/android-retroarch/typex2_libretro_android.so"], built: 1763040000 }],
  tools: [
    { id: "x2recomp", name: "x2recomp", path: "build/recompiler/x2recomp", source: "kit", for_what: "Every build", download: null, hint: "" },
    { id: "player", name: "x2-player", path: "build/x2-player", source: "kit", for_what: "Playing on this computer", download: null, hint: "" },
    { id: "cc", name: "C compiler (clang)", path: "tools/llvm-23.1.4/bin/clang", source: "downloaded", for_what: "Playing on this computer", download: "llvm", hint: "" },
    { id: "cmake", name: "CMake", path: null, source: null, for_what: "Exports", download: "cmake", hint: "cmake.org" },
    { id: "ninja", name: "Ninja", path: null, source: null, for_what: "Exports", download: "ninja", hint: "ninja-build.org" },
    { id: "ndk", name: "Android NDK", path: null, source: null, for_what: "Android exports", download: "ndk", hint: "Android Studio → SDK Manager → NDK" },
  ],
  packages: [
    { id: "llvm", name: "LLVM clang", version: "23.1.4", size: 0, for_what: "Compiling games to play on this computer" },
    { id: "cmake", name: "CMake", version: "4.5.0", size: 92795915, for_what: "Exports" },
    { id: "ninja", name: "Ninja", version: "1.13.2", size: 314051, for_what: "Exports" },
    { id: "ndk", name: "Android NDK", version: "r30", size: 974984488, for_what: "Android exports" },
  ],
  tools_size: 342000000,
};

export async function mockInvoke(cmd: string, args?: Record<string, unknown>): Promise<unknown> {
  if (cmd === "load_state") return structuredClone(state);
  if (cmd === "game_art") return null; // the preview has no art: cards show their plain background
  if (cmd === "save_language") state.language = args?.language as string;
  if (cmd === "allow_all_files" && state.android) state.android.all_files = true;
  if (cmd === "save_update_check") state.check_updates = args?.enabled as boolean;
  if (cmd === "list_devices") return { adb: true, devices: [{ serial: "R8YL10WF7EZ", model: "SM X130", state: "device" }, { serial: "192.168.1.20:5555", model: "192.168.1.20:5555", state: "unauthorized" }] };
  if (cmd === "send_export") return new Promise((done) => setTimeout(() => done(args?.target === "android-packs" ? "packs" : "core"), 1200));
  // ?update in the preview: a newer release, with notes that must not render HTML or images
  if (cmd === "check_release")
    return new URLSearchParams(window.location.search).has("update")
      ? { version: "0.2.0", tag: "v0.2.0", newer: true, notes: "## What's new\n\n- Switch games in RetroArch without restarting it.\n- **Linux** reports welcome.\n\n<script>window.untrusted = true</script>\n![remote](https://example.com/x.png)" }
      : { version: "0.1.0", tag: "v0.1.0", newer: false, notes: "" };
  if (cmd === "save_game_settings") {
    const g = state.games.find((x) => x.id === args?.id);
    if (g) g.settings = args?.settings as GameSettings;
  }
  if (cmd === "scan_folder") return [{ id: "kof13", title: "The King of Fighters XIII", new: false }, { id: "bbct", title: "BlazBlue: Calamity Trigger", new: true }];
  if (cmd === "read_pad") return new Promise((done) => setTimeout(() => done("north"), 800)); // a button pressed
  return null;
}

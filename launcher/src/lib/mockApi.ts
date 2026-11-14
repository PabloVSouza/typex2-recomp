import type { AppStateData, GameInfo } from "@/lib/types";

/** Browser preview only: a library with a few games in different states. */
const kof: GameInfo["layouts"] = [{ label: "Game default", pad: "" }];
const catalog: GameInfo[] = [
  { id: "kof12", title: "The King of Fighters XII", board: "Type X2", year: 2009, publisher: "SNK Playmore", layouts: kof },
  { id: "kof13", title: "The King of Fighters XIII", board: "Type X2", year: 2010, publisher: "SNK Playmore", layouts: kof },
  { id: "kof13cl", title: "The King of Fighters XIII Climax", board: "Type X2", year: 2012, publisher: "SNK Playmore", layouts: kof },
  { id: "ssf4ae", title: "Super Street Fighter IV Arcade Edition", board: "Type X2", year: 2010, publisher: "Capcom", layouts: [...kof, { label: "8-button stick (L1 / L2 rows)", pad: "l1=1,west=2,north=3,r1=123,l2=4,south=5,east=6,r2=456" }] },
  { id: "bbcs", title: "BlazBlue: Continuum Shift", board: "Type X2", year: 2009, publisher: "Arc System Works", layouts: kof },
  { id: "bbct", title: "BlazBlue: Calamity Trigger", board: "Type X2", year: 2008, publisher: "Arc System Works", layouts: kof },
  { id: "kof98um", title: "The King of Fighters '98 Ultimate Match", board: "Type X", year: 2008, publisher: "SNK Playmore", layouts: kof },
  { id: "kofmira", title: "The King of Fighters: Maximum Impact Regulation A", board: "Type X2", year: 2007, publisher: "SNK Playmore", layouts: kof },
];
const settings = { fullscreen: false, fps: false, pad: "", scale: 0, vsync: false, log_level: 0, env: "" };
const entry = (id: string, state: "none" | "ready" | "stale") => ({
  id, path: `/Games/${catalog.find((g) => g.id === id)!.title}`, settings, info: catalog.find((g) => g.id === id)!, found: true,
  library: { state, built: state === "none" ? null : { built: 1763000000, seconds: 74, size: 11228104, functions: 10967 } },
});
const state: AppStateData = {
  version: "0.1.0", os: "macos", arch: "aarch64", data: "~/Library/Application Support/io.github.typex2-recomp.launcher",
  kit: { source: "~/typex2-recomp", x2recomp: "build/recompiler/x2recomp", player: "build/x2-player", kind: "checkout" },
  can_play: true, catalog, games: [entry("kof13", "ready"), entry("ssf4ae", "none"), entry("bbcs", "stale"), entry("kof98um", "ready")],
  ndk: "", jobs: 0, job: null, running: null,
  targets: [
    { id: "android-retroarch", name: "RetroArch core for Android", description: "typex2_libretro_android.so with the chosen games. Copy it to RetroArch/downloads/ on the device, then Load Core → Install or Restore a Core.", needs: ["x2recomp", "cmake", "ninja", "ndk"] },
    { id: "android-app", name: "Android app", description: "typex2.apk: the standalone app (ES-DE can launch it) with the chosen games. Needs the Android SDK's build tools next to the NDK.", needs: ["x2recomp", "cmake", "ninja", "ndk"] },
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

export async function mockInvoke(cmd: string, _args?: Record<string, unknown>): Promise<unknown> {
  if (cmd === "load_state") return structuredClone(state);
  if (cmd === "read_pad") return new Promise((done) => setTimeout(() => done("north"), 800)); // a button pressed
  return null;
}

/** The backend's names and descriptions (layouts, export targets, tools, packages) in the
 *  launcher's language, by id; the backend's English for ids this list does not know. */
import type { TFunction } from "i18next";
import type { Layout, Package, Target, Tool } from "@/lib/types";

export function layoutName(t: TFunction, l: Layout): string {
  switch (l.id) {
    case "default": return t("layouts.default");
    case "stick-kof": return t("layouts.stickKof");
    case "stick-sf4": return t("layouts.stickSf4");
    case "stick-bb": return t("layouts.stickBb");
    default: return l.label;
  }
}

export function targetName(t: TFunction, x: Target): string {
  switch (x.id) {
    case "android-retroarch": return t("targets.androidRetroarch.name");
    case "android-app": return t("targets.androidApp.name");
    case "host-retroarch": return t("targets.hostRetroarch.name");
    default: return x.name;
  }
}

export function targetDescription(t: TFunction, x: Target): string {
  switch (x.id) {
    case "android-retroarch": return t("targets.androidRetroarch.description");
    case "android-app": return t("targets.androidApp.description");
    case "host-retroarch": return t("targets.hostRetroarch.description");
    default: return x.description;
  }
}

export function toolName(t: TFunction, x: Tool): string {
  return x.id === "cc" ? t("tools.names.cc") : x.name;
}

/** What a tool is needed for. */
export function toolUse(t: TFunction, x: Tool): string {
  switch (x.id) {
    case "x2recomp": return t("tools.use.every");
    case "player": case "cc": return t("tools.use.play");
    case "cmake": case "ninja": return t("tools.use.exports");
    case "ndk": return t("tools.use.android");
    default: return x.for_what;
  }
}

/** Where to get a tool that is missing; `os` is the backend's (macos, windows, linux). */
export function toolHint(t: TFunction, x: Tool, os: string): string {
  switch (x.id) {
    case "x2recomp": return t("tools.hints.kit", { command: "cmake --build build --target x2recomp" });
    case "player": return t("tools.hints.kit", { command: "cmake --build build --target x2-player" });
    case "cc": return os === "macos" ? t("tools.hints.ccMac") : os === "windows" ? t("tools.hints.ccWindows") : t("tools.hints.ccLinux");
    case "ndk": return t("tools.hints.ndk");
    default: return x.hint;
  }
}

export function packageUse(t: TFunction, p: Package): string {
  switch (p.id) {
    case "llvm": return t("tools.use.compile");
    case "cmake": case "ninja": return t("tools.use.exports");
    case "ndk": return t("tools.use.android");
    default: return p.for_what;
  }
}

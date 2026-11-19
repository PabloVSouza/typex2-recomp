import type { ReactNode } from "react";
import { Boxes, Gamepad2, Info, Library, ScrollText, Wrench } from "lucide-react";
import { useTranslation } from "react-i18next";
import { Dot } from "@/components/sidebar/Dot";
import { NavItem } from "@/components/sidebar/NavItem";
import { useAppState, useNarrow } from "@/hooks";
import { cn } from "@/lib";
import { useUi, type Page } from "@/stores";

export function Sidebar() {
  const { t } = useTranslation();
  const { data } = useAppState();
  const { page, gameId, go, menuHidden } = useUi();
  const narrow = useNarrow();
  const hidden = menuHidden && !narrow; // upright, the rail stays
  const at = (p: Page) => page === p;
  // the Android app plays games built elsewhere: no exports or build tools there
  const desktop = data?.os !== "android";
  const all: [Page, string, ReactNode][] = [
    ["export", t("nav.export"), <Boxes key="e" />],
    ["tools", t("nav.tools"), <Wrench key="t" />],
    ["log", t("nav.log"), <ScrollText key="l" />],
    ["about", t("nav.about"), <Info key="a" />],
  ];
  const tabs = desktop ? all : all.filter(([p]) => p !== "export" && p !== "tools");
  return (
    // Fixed at the left, the page padded beside it (App). Full-screen mode (menuHidden) slides it
    // away with a transform: the GPU moves it, nothing is laid out again while it moves (animating its
    // width re-laid the whole page every frame). Upright (an icon rail) it stays. The logo and the
    // button that hides it are BrandBar, fixed above: they don't move.
    <aside
      className={cn(
        "drag fixed inset-y-0 left-0 z-20 w-64 border-r border-sidebar-border bg-sidebar transition-transform duration-300 ease-out will-change-transform max-md:w-16",
        hidden && "-translate-x-full",
      )}
      aria-hidden={hidden || undefined}
      inert={hidden || undefined}
      data-tauri-drag-region
    >
      <div className={cn("flex h-full w-64 flex-col px-3 pb-4 max-md:w-16 max-md:px-2", data?.os === "macos" ? "pt-11" : "pt-5")}>
        <div className="mb-7 h-9 max-md:mb-5" /* BrandBar sits here */ />
        <nav className="no-drag -mx-1 flex min-h-0 flex-1 flex-col gap-0.5 overflow-x-hidden overflow-y-auto px-1">
          <NavItem label={t("nav.library")} icon={<Library />} active={at("library")} onClick={() => go("library")} />
          {data && data.games.length > 0 && (
            <div className="mt-4 flex flex-col gap-0.5 max-md:hidden">
              <p className="px-3 pb-1 text-[11px] uppercase tracking-wider text-muted-foreground">{t("nav.games")}</p>
              {data.games.map((g) => (
                <NavItem
                  key={g.id}
                  label={g.title}
                  icon={<Gamepad2 />}
                  active={at("game") && gameId === g.id}
                  onClick={() => go("game", g.id)}
                  badge={data.running === g.id ? <Dot className="animate-pulse bg-lime" /> : g.library.state === "ready" ? <Dot className="bg-lime/60" /> : undefined}
                />
              ))}
            </div>
          )}
          <div className="mt-4 flex flex-col gap-0.5">
            {tabs.map(([p, label, icon]) => (
              <NavItem key={p} label={label} icon={icon} active={at(p)} onClick={() => go(p)} badge={p === "log" && data?.job ? <Dot className="animate-pulse bg-cyan" /> : undefined} />
            ))}
          </div>
        </nav>
        <div className="no-drag mt-3 text-center text-[11px] text-muted-foreground max-md:text-[9px]">v{data?.version}</div>
      </div>
    </aside>
  );
}

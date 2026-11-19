import type { ReactNode } from "react";
import { Boxes, Gamepad2, Info, Library, PanelLeftClose, ScrollText, Wrench } from "lucide-react";
import { useTranslation } from "react-i18next";
import { Brand } from "@/components/sidebar/Brand";
import { Dot } from "@/components/sidebar/Dot";
import { NavItem } from "@/components/sidebar/NavItem";
import { useAppState } from "@/hooks";
import { cn } from "@/lib";
import { useUi, type Page } from "@/stores";

export function Sidebar() {
  const { t } = useTranslation();
  const { data } = useAppState();
  const { page, gameId, go, setMenuHidden } = useUi();
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
    <aside className={cn("drag flex w-64 shrink-0 flex-col border-r border-sidebar-border bg-sidebar/70 px-3 pb-4 backdrop-blur-sm max-md:w-16 max-md:px-2", data?.os === "macos" ? "pt-11" : "pt-5")} data-tauri-drag-region>
      <div className="mb-7 flex items-center justify-between gap-2 px-2 max-md:mb-5 max-md:justify-center max-md:px-0">
        <Brand />
        <button
          type="button"
          onClick={() => setMenuHidden(true)}
          title={t("nav.hideMenu")}
          aria-label={t("nav.hideMenu")}
          className="no-drag rounded-lg p-1.5 text-muted-foreground transition hover:bg-white/5 hover:text-foreground max-md:hidden [&_svg]:size-4"
        >
          <PanelLeftClose />
        </button>
      </div>
      <nav className="no-drag flex min-h-0 flex-1 flex-col gap-0.5 overflow-y-auto">
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
    </aside>
  );
}

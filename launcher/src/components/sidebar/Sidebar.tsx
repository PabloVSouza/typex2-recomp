import type { ReactNode } from "react";
import { Boxes, Gamepad2, Info, Library, ScrollText, Wrench } from "lucide-react";
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
  const { page, gameId, go } = useUi();
  const at = (p: Page) => page === p;
  const tabs: [Page, string, ReactNode][] = [
    ["export", t("nav.export"), <Boxes key="e" />],
    ["tools", t("nav.tools"), <Wrench key="t" />],
    ["log", t("nav.log"), <ScrollText key="l" />],
    ["about", t("nav.about"), <Info key="a" />],
  ];
  return (
    <aside className={cn("drag flex w-64 shrink-0 flex-col border-r border-sidebar-border bg-sidebar/70 px-3 pb-4 backdrop-blur-sm", data?.os === "macos" ? "pt-11" : "pt-5")} data-tauri-drag-region>
      <div className="mb-7 px-2"><Brand /></div>
      <nav className="no-drag flex min-h-0 flex-1 flex-col gap-0.5 overflow-y-auto">
        <NavItem label={t("nav.library")} icon={<Library />} active={at("library")} onClick={() => go("library")} />
        {data && data.games.length > 0 && (
          <div className="mt-4 flex flex-col gap-0.5">
            <p className="px-3 pb-1 text-[11px] uppercase tracking-wider text-muted-foreground">{t("nav.games")}</p>
            {data.games.map((g) => (
              <NavItem
                key={g.id}
                label={g.info?.title ?? g.id}
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
      <div className="no-drag mt-3 text-center text-[11px] text-muted-foreground">v{data?.version}</div>
    </aside>
  );
}

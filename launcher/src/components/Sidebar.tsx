import type { ReactNode } from "react";
import { Boxes, Gamepad2, Info, Library, ScrollText, Wrench } from "lucide-react";
import { useAppState } from "@/hooks";
import { useUi, type Page } from "@/stores";
import { cn } from "@/lib/utils";

function NavItem({ label, icon, active, onClick, badge }: { label: string; icon: ReactNode; active: boolean; onClick: () => void; badge?: ReactNode }) {
  return (
    <button
      type="button"
      onClick={onClick}
      aria-current={active ? "page" : undefined}
      className={cn(
        "flex w-full min-w-0 items-center gap-3 rounded-lg px-3 py-2 text-left text-sm transition [&_svg]:size-4 [&_svg]:shrink-0",
        active ? "bg-sidebar-accent text-sidebar-accent-foreground [&_svg]:text-cyan" : "text-muted-foreground hover:bg-white/5 hover:text-foreground",
      )}
    >
      {icon}
      <span className="truncate">{label}</span>
      {badge && <span className="ml-auto shrink-0">{badge}</span>}
    </button>
  );
}

const Dot = ({ className }: { className: string }) => <span className={cn("block size-2 rounded-full", className)} />;

export function Brand() {
  return (
    <div className="flex items-center gap-2.5">
      <div className="grid size-8 place-items-center rounded-lg bg-gradient-to-br from-amber to-pink font-display text-sm font-bold text-[#1a1206]">X</div>
      <div className="leading-tight">
        <div className="font-display text-sm font-bold tracking-[0.18em]">TYPE X</div>
        <div className="font-display text-[10px] tracking-[0.3em] text-muted-foreground">RECOMP</div>
      </div>
    </div>
  );
}

export function Sidebar() {
  const { data } = useAppState();
  const { page, gameId, go } = useUi();
  const at = (p: Page) => page === p;
  const tabs: [Page, string, ReactNode][] = [
    ["export", "Export", <Boxes key="e" />],
    ["tools", "Build tools", <Wrench key="t" />],
    ["log", "Log", <ScrollText key="l" />],
    ["about", "About", <Info key="a" />],
  ];
  return (
    <aside className={cn("drag flex w-64 shrink-0 flex-col border-r border-sidebar-border bg-sidebar/70 px-3 pb-4 backdrop-blur-sm", data?.os === "macos" ? "pt-11" : "pt-5")} data-tauri-drag-region>
      <div className="mb-7 px-2"><Brand /></div>
      <nav className="no-drag flex min-h-0 flex-1 flex-col gap-0.5 overflow-y-auto">
        <NavItem label="Library" icon={<Library />} active={at("library")} onClick={() => go("library")} />
        {data && data.games.length > 0 && (
          <div className="mt-4 flex flex-col gap-0.5">
            <p className="px-3 pb-1 text-[11px] uppercase tracking-wider text-muted-foreground">Games</p>
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

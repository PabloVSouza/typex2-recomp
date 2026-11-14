import type { ReactNode } from "react";
import { cn } from "@/lib";

export function NavItem({ label, icon, active, onClick, badge }: { label: string; icon: ReactNode; active: boolean; onClick: () => void; badge?: ReactNode }) {
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

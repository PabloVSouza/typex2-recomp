import type { ReactNode } from "react";
import { cn } from "@/lib";

export function NavItem({ label, icon, active, onClick, badge }: { label: string; icon: ReactNode; active: boolean; onClick: () => void; badge?: ReactNode }) {
  return (
    <button
      type="button"
      onClick={onClick}
      aria-current={active ? "page" : undefined}
      title={label}
      className={cn(
        "relative flex w-full min-w-0 items-center gap-3 rounded-lg px-3 py-2 text-left text-sm transition [&_svg]:size-4 [&_svg]:shrink-0 max-md:justify-center max-md:px-0 max-md:py-2.5 max-md:[&_svg]:size-5",
        active ? "bg-sidebar-accent text-sidebar-accent-foreground [&_svg]:text-cyan" : "text-muted-foreground hover:bg-white/5 hover:text-foreground",
      )}
    >
      {icon}
      <span className="truncate max-md:sr-only">{label}</span>
      {badge && <span className="ml-auto shrink-0 max-md:absolute max-md:right-1.5 max-md:top-1.5">{badge}</span>}
    </button>
  );
}

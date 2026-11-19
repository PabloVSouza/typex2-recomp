import type { ReactNode } from "react";

/** A small chip over a card's art: an icon and a short value. */
export function InfoChip({ icon, children, title }: { icon: ReactNode; children: ReactNode; title?: string }) {
  return (
    <span title={title} className="inline-flex items-center gap-1 rounded-md bg-black/45 px-1.5 py-0.5 text-[11px] font-medium text-white/85 backdrop-blur-sm [&_svg]:size-3">
      {icon}
      {children}
    </span>
  );
}

import type { ReactNode } from "react";

/** A small heading between sections. */
export function SectionHeading({ children }: { children: ReactNode }) {
  return <h2 className="mb-3 mt-8 font-display text-xs font-semibold uppercase tracking-[0.2em] text-muted-foreground">{children}</h2>;
}

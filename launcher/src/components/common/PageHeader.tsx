import type { ReactNode } from "react";

/** A page's title, with the kicker above it, a description and actions on the right. */
export function PageHeader({ title, kicker, description, actions }: { title: ReactNode; kicker?: ReactNode; description?: ReactNode; actions?: ReactNode }) {
  return (
    <header className="mb-7 flex flex-wrap items-end justify-between gap-4">
      <div className="min-w-0">
        {kicker && <div className="mb-1 font-display text-xs font-medium uppercase tracking-[0.25em] text-cyan">{kicker}</div>}
        <h1 className="font-display text-3xl font-bold tracking-wide">{title}</h1>
        {description && <p className="mt-2 max-w-2xl text-sm leading-relaxed text-muted-foreground">{description}</p>}
      </div>
      {actions && <div className="flex shrink-0 flex-wrap items-center gap-2">{actions}</div>}
    </header>
  );
}

import type { ReactNode } from "react";

/** A placeholder for an empty list. */
export function Empty({ icon, title, children }: { icon: ReactNode; title: string; children?: ReactNode }) {
  return (
    <div className="surface flex flex-col items-center rounded-2xl px-6 py-14 text-center">
      <div className="mb-4 text-cyan [&_svg]:size-10">{icon}</div>
      <div className="font-display text-lg font-semibold">{title}</div>
      {children && <div className="mt-2 max-w-md text-sm text-muted-foreground">{children}</div>}
    </div>
  );
}

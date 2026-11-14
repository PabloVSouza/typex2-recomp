import type { ReactNode } from "react";
import { cn } from "@/lib";

/** A setting or fact: title and description on the left, controls on the right (below on phones). */
export function Row({ title, description, children, disabled }: { title: ReactNode; description?: ReactNode; children?: ReactNode; disabled?: boolean }) {
  return (
    <div className={cn("flex flex-col items-stretch gap-3 px-5 py-3.5 sm:flex-row sm:items-center sm:justify-between sm:gap-6", disabled && "pointer-events-none opacity-40")}>
      <div className="min-w-0">
        <div className="text-sm font-medium">{title}</div>
        {description && <div className="mt-0.5 text-xs leading-relaxed text-muted-foreground">{description}</div>}
      </div>
      <div className="flex min-w-0 shrink-0 flex-wrap items-center gap-2 [&>*]:max-w-full">{children}</div>
    </div>
  );
}

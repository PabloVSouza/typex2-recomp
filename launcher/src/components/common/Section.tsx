import type { ReactNode } from "react";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui";
import { cn } from "@/lib";

/** A card of rows with an optional title. */
export function Section({ title, description, children, className }: { title?: string; description?: ReactNode; children: ReactNode; className?: string }) {
  return (
    <Card className={cn("surface mb-5 gap-0 py-0 ring-0", className)}>
      {(title || description) && (
        <CardHeader className="px-5 pb-1 pt-4">
          {title && <CardTitle className="font-display text-xs font-semibold uppercase tracking-[0.2em] text-amber">{title}</CardTitle>}
          {description && <CardDescription className="text-xs leading-relaxed">{description}</CardDescription>}
        </CardHeader>
      )}
      <CardContent className="px-0 pb-1">{children}</CardContent>
    </Card>
  );
}

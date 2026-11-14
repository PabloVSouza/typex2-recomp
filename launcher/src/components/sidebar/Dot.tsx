import { cn } from "@/lib";

/** A status light next to a sidebar item. */
export function Dot({ className }: { className: string }) {
  return <span className={cn("block size-2 rounded-full", className)} />;
}

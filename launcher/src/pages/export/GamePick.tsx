import { Check } from "lucide-react";
import { cn } from "@/lib";

/** A game to include in the export, on or off. */
export function GamePick({ title, on, onClick }: { title: string; on: boolean; onClick: () => void }) {
  return (
    <button
      type="button"
      onClick={onClick}
      aria-pressed={on}
      className={cn("flex items-center gap-3 rounded-lg px-3 py-2.5 text-left text-sm transition", on ? "bg-cyan/10" : "bg-white/[0.03] text-muted-foreground")}
    >
      <span className={cn("grid size-4 shrink-0 place-items-center rounded border", on ? "border-cyan bg-cyan text-[#0b0a14]" : "border-input")}>
        {on && <Check className="size-3" strokeWidth={3} />}
      </span>
      <span className="truncate">{title}</span>
    </button>
  );
}

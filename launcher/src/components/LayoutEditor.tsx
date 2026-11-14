import { useState } from "react";
import { Crosshair, LoaderCircle, RotateCcw } from "lucide-react";
import { toast } from "sonner";
import { Badge, Button } from "@/components/ui";
import { api } from "@/lib/api";
import {
  actionsFor, assign, buttonLabel, buttonsFor, isDefault, layoutOf, padOf, PAD_BUTTONS, resetAction,
  type PadButton,
} from "@/lib/controls";

/** One row per cabinet action: the controller buttons pressing it, Assign (the next button pressed
 *  on the controller), and back to the game's default. Saves a full X2_PAD string. */
export function LayoutEditor({ game, pad, onChange }: { game: string; pad: string; onChange: (pad: string) => void }) {
  const layout = layoutOf(game, pad);
  const actions = actionsFor(game);
  const [waiting, setWaiting] = useState<string | null>(null);
  const def = layoutOf(game, "");

  const press = async (keys: string) => {
    setWaiting(keys);
    try {
      const button = await api.readPad();
      if (button) onChange(padOf(assign(layout, button as PadButton, keys)));
    } catch (error) {
      toast.error("Could not read the controller", { description: String(error) });
    } finally {
      setWaiting(null);
    }
  };

  // buttons pressing something no row names (an X2_PAD typed by hand, e.g. "a+c")
  const known = new Set(actions.map((a) => [...a.keys].sort().join("")));
  const other = PAD_BUTTONS.filter((b) => layout[b.id] && !known.has(layout[b.id]));

  return (
    <div className="divide-y divide-border/60">
      {actions.map((a) => {
        const buttons = buttonsFor(layout, a.keys);
        const changed = buttonsFor(def, a.keys).join() !== buttons.join();
        return (
          <div key={a.keys} className="flex flex-wrap items-center justify-between gap-x-4 gap-y-2 px-5 py-2.5">
            <div className="text-sm">
              <span className="font-medium">{a.label}</span>
              {a.hint && <span className="ml-2 text-xs text-muted-foreground">{a.hint}</span>}
            </div>
            <div className="flex flex-wrap items-center gap-2">
              {waiting === a.keys ? (
                <span className="flex items-center gap-2 text-xs text-cyan">
                  <LoaderCircle className="size-3.5 animate-spin" /> Press a button on your controller…
                </span>
              ) : buttons.length ? (
                buttons.map((b) => (
                  <Badge key={b} variant={changed ? "default" : "secondary"} className="font-mono text-[11px]">
                    {buttonLabel(b)}
                  </Badge>
                ))
              ) : (
                <span className="text-xs text-muted-foreground">Not assigned</span>
              )}
              <Button variant="secondary" size="sm" disabled={waiting !== null} onClick={() => press(a.keys)}>
                <Crosshair /> Assign
              </Button>
              <Button
                variant="ghost"
                size="icon-sm"
                title="Back to the game's default"
                disabled={!changed || waiting !== null}
                onClick={() => onChange(padOf(resetAction(game, layout, a.keys)))}
              >
                <RotateCcw />
              </Button>
            </div>
          </div>
        );
      })}
      {other.length > 0 && (
        <div className="px-5 py-2.5 text-xs text-muted-foreground">
          Other: {other.map((b) => `${buttonLabel(b.id)} = ${layout[b.id]}`).join(" · ")}
        </div>
      )}
      <div className="flex items-center justify-between px-5 py-2.5">
        <span className="text-xs text-muted-foreground">Start, Back (coin; hold for the test menu) and the d-pad keep their roles.</span>
        <Button variant="secondary" size="sm" disabled={isDefault(game, layout) || waiting !== null} onClick={() => onChange("")}>
          <RotateCcw /> Reset all
        </Button>
      </div>
    </div>
  );
}

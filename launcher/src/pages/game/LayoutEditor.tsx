import { useState } from "react";
import { RotateCcw } from "lucide-react";
import { useTranslation } from "react-i18next";
import { toast } from "sonner";
import { Button } from "@/components/ui";
import { api } from "@/lib";
import { actionsFor, assign, buttonLabel, isDefault, layoutOf, padOf, PAD_BUTTONS, resetAction, type PadButton } from "@/lib/data";
import { errorText } from "@/lib";
import { ActionRow } from "@/pages/game/ActionRow";

/** One row per cabinet action: Assign takes the next button pressed on the controller. Saves a
 *  full X2_PAD string. */
export function LayoutEditor({ game, pad, onChange }: { game: string; pad: string; onChange: (pad: string) => void }) {
  const { t } = useTranslation();
  const layout = layoutOf(game, pad);
  const defaults = layoutOf(game, "");
  const actions = actionsFor(game);
  const [waiting, setWaiting] = useState<string | null>(null);

  const press = async (keys: string) => {
    setWaiting(keys);
    try {
      const button = await api.readPad();
      if (button) onChange(padOf(assign(layout, button as PadButton, keys)));
    } catch (error) {
      toast.error(t("controls.readFailed"), { description: errorText(error) });
    } finally {
      setWaiting(null);
    }
  };

  // buttons pressing something no row names (an X2_PAD typed by hand, e.g. "a+c")
  const known = new Set(actions.map((a) => [...a.keys].sort().join("")));
  const other = PAD_BUTTONS.filter((b) => layout[b.id] && !known.has(layout[b.id]));

  return (
    <div className="divide-y divide-border/60">
      {actions.map((a) => (
        <ActionRow
          key={a.keys}
          action={a}
          layout={layout}
          defaults={defaults}
          waiting={waiting === a.keys}
          busy={waiting !== null}
          onAssign={() => press(a.keys)}
          onReset={() => onChange(padOf(resetAction(game, layout, a.keys)))}
        />
      ))}
      {other.length > 0 && (
        <div className="px-5 py-2.5 text-xs text-muted-foreground">
          {t("controls.other")}: {other.map((b) => `${buttonLabel(b.id)} = ${layout[b.id]}`).join(" · ")}
        </div>
      )}
      <div className="flex flex-wrap items-center justify-between gap-2 px-5 py-2.5">
        <span className="text-xs text-muted-foreground">{t("controls.fixed")}</span>
        <Button variant="secondary" size="sm" disabled={isDefault(game, layout) || waiting !== null} onClick={() => onChange("")}>
          <RotateCcw /> {t("controls.resetAll")}
        </Button>
      </div>
    </div>
  );
}

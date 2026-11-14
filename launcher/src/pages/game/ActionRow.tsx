import { Crosshair, LoaderCircle, RotateCcw } from "lucide-react";
import { useTranslation } from "react-i18next";
import { Badge, Button } from "@/components/ui";
import { buttonLabel, buttonsFor, type Action, type Layout } from "@/lib/data";

/** One cabinet action: the controller buttons pressing it, Assign and back to the default. */
export function ActionRow({ action, layout, defaults, waiting, busy, onAssign, onReset }: {
  action: Action; layout: Layout; defaults: Layout; waiting: boolean; busy: boolean; onAssign: () => void; onReset: () => void;
}) {
  const { t } = useTranslation();
  const buttons = buttonsFor(layout, action.keys);
  const changed = buttonsFor(defaults, action.keys).join() !== buttons.join();
  return (
    <div className="flex flex-wrap items-center justify-between gap-x-4 gap-y-2 px-5 py-2.5">
      <div className="text-sm">
        <span className="font-medium">{action.nameKey ? t(action.nameKey) : action.name}</span>
        {action.hint && <span className="ml-2 text-xs text-muted-foreground">{t(action.hint)}</span>}
      </div>
      <div className="flex flex-wrap items-center gap-2">
        {waiting ? (
          <span className="flex items-center gap-2 text-xs text-cyan">
            <LoaderCircle className="size-3.5 animate-spin" /> {t("controls.pressButton")}
          </span>
        ) : buttons.length ? (
          buttons.map((b) => (
            <Badge key={b} variant={changed ? "default" : "secondary"} className="font-mono text-[11px]">
              {buttonLabel(b)}
            </Badge>
          ))
        ) : (
          <span className="text-xs text-muted-foreground">{t("controls.notAssigned")}</span>
        )}
        <Button variant="secondary" size="sm" disabled={busy} onClick={onAssign}>
          <Crosshair /> {t("controls.assign")}
        </Button>
        <Button variant="ghost" size="icon-sm" title={t("controls.resetOne")} aria-label={t("controls.resetOne")} disabled={!changed || busy} onClick={onReset}>
          <RotateCcw />
        </Button>
      </div>
    </div>
  );
}

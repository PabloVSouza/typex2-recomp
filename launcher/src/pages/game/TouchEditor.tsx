import { useTranslation } from "react-i18next";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui";
import { touchActions, touchSlots, touchSpec } from "@/lib/data";

/** A custom touch pad: two rows of four slots, laid out like the pad on screen (the top row a
 *  little to the right), each pressing one of the game's buttons or combinations, or nothing. */
export function TouchEditor({ game, spec, onChange }: { game: string; spec: string; onChange: (spec: string) => void }) {
  const { t } = useTranslation();
  const slots = touchSlots(spec);
  const none = "-";
  const options = [
    { value: none, label: t("game.display.touchSlotNone") },
    ...touchActions(game).map((a) => ({ value: a.keys, label: a.name ?? (a.nameKey ? t(a.nameKey) : a.keys) })),
  ];
  const set = (i: number, keys: string) => {
    const next = [...slots];
    next[i] = keys === none ? "" : keys;
    onChange(touchSpec(next) || "1=-"); // an empty custom pad stays custom
  };
  const row = (from: number, shift: boolean) => (
    <div className={`grid grid-cols-4 gap-2 ${shift ? "pl-6" : "pr-6"}`}>
      {[0, 1, 2, 3].map((k) => {
        const i = from + k;
        return (
          <Select key={i} value={slots[i] || none} items={options} onValueChange={(v) => v !== null && set(i, String(v))}>
            <SelectTrigger className="w-full bg-secondary" aria-label={t("game.display.touchSlot", { n: i + 1 })}>
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              {options.map((o) => (
                <SelectItem key={o.value} value={o.value}>{o.label}</SelectItem>
              ))}
            </SelectContent>
          </Select>
        );
      })}
    </div>
  );
  return (
    <div className="space-y-2 px-5 pb-4">
      <p className="text-xs text-muted-foreground">{t("game.display.touchCustomText")}</p>
      {row(0, true)}
      {row(4, false)}
    </div>
  );
}

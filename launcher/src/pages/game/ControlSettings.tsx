import { useTranslation } from "react-i18next";
import { Section, SelectRow } from "@/components/common";
import { layoutName, layoutOf, padOf } from "@/lib/data";
import type { GameEntry, GameSettings } from "@/lib";
import { LayoutEditor } from "@/pages/game/LayoutEditor";

const CUSTOM = "__custom";

/** A layout preset, then button by button. */
export function ControlSettings({ game, s, set }: { game: GameEntry; s: GameSettings; set: (patch: Partial<GameSettings>) => void }) {
  const { t } = useTranslation();
  const layouts = game.info?.layouts ?? [];
  const same = (a: string, b: string) => padOf(layoutOf(game.id, a)) === padOf(layoutOf(game.id, b));
  const preset = layouts.find((l) => same(l.pad, s.pad));
  return (
    <Section title={t("controls.title")} description={t("controls.description")}>
      <SelectRow
        title={t("controls.layout")}
        description={t("controls.layoutText")}
        value={preset ? preset.pad : CUSTOM}
        options={[...layouts.map((l) => ({ value: l.pad, label: layoutName(t, l) })), ...(preset ? [] : [{ value: CUSTOM, label: t("controls.custom") }])]}
        onChange={(v) => v !== CUSTOM && set({ pad: v })}
      />
      <LayoutEditor game={game.id} pad={s.pad} onChange={(pad) => set({ pad })} />
    </Section>
  );
}

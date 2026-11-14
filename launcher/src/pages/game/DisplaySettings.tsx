import { useTranslation } from "react-i18next";
import { Section, SelectRow, SwitchRow } from "@/components/common";
import type { GameSettings } from "@/lib";

export function DisplaySettings({ s, set }: { s: GameSettings; set: (patch: Partial<GameSettings>) => void }) {
  const { t } = useTranslation();
  return (
    <Section title={t("game.display.title")}>
      <SwitchRow title={t("game.display.fullscreen")} checked={s.fullscreen} onChange={(fullscreen) => set({ fullscreen })} />
      <SwitchRow title={t("game.display.vsync")} description={t("game.display.vsyncText")} checked={s.vsync} onChange={(vsync) => set({ vsync })} />
      <SwitchRow title={t("game.display.fps")} description={t("game.display.fpsText")} checked={s.fps} onChange={(fps) => set({ fps })} />
      <SelectRow
        title={t("game.display.scale")}
        description={t("game.display.scaleText")}
        value={s.scale}
        options={[
          { value: 0, label: t("game.display.scaleAuto") },
          { value: 1, label: t("game.display.scaleOriginal") },
          { value: 2, label: "2×" },
          { value: 3, label: "3×" },
          { value: 4, label: "4×" },
        ]}
        onChange={(scale) => set({ scale })}
      />
    </Section>
  );
}

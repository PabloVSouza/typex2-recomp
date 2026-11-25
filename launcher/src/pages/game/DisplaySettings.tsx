import { useTranslation } from "react-i18next";
import { Section, SelectRow, SwitchRow } from "@/components/common";
import { useAppState } from "@/hooks";
import type { GameSettings } from "@/lib";
import { touchDefault, touchHasNeoGeo } from "@/lib/data";
import { TouchEditor } from "@/pages/game/TouchEditor";

type TouchLayout = "default" | "neogeo" | "custom";

export function DisplaySettings({ game, s, set }: { game: string; s: GameSettings; set: (patch: Partial<GameSettings>) => void }) {
  const { t } = useTranslation();
  const { data } = useAppState();
  // Android: games are always full screen and paced by the display; touch screens get the touch pad
  const android = !!data?.android;
  const layout: TouchLayout = !s.touch_buttons ? "default" : s.touch_buttons === "neogeo" ? "neogeo" : "custom";
  const touchLayouts: { value: TouchLayout; label: string }[] = [
    { value: "default", label: t("game.display.touchButtonsDefault") },
    ...(touchHasNeoGeo(game) ? [{ value: "neogeo" as const, label: t("game.display.touchButtonsNeoGeo") }] : []),
    { value: "custom", label: t("game.display.touchButtonsCustom") },
  ];
  const chooseLayout = (v: TouchLayout) =>
    set({ touch_buttons: v === "default" ? "" : v === "neogeo" ? "neogeo" : touchDefault(game) }); // custom starts from the game's own
  return (
    <Section title={t("game.display.title")}>
      {!android && <SwitchRow title={t("game.display.fullscreen")} checked={s.fullscreen} onChange={(fullscreen) => set({ fullscreen })} />}
      {!android && <SwitchRow title={t("game.display.vsync")} description={t("game.display.vsyncText")} checked={s.vsync} onChange={(vsync) => set({ vsync })} />}
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
      {android && (
        <SelectRow
          title={t("game.display.touch")}
          description={t("game.display.touchText")}
          value={s.touch}
          options={[
            { value: 0, label: t("game.display.touchAuto") },
            { value: 1, label: t("game.display.touchAlways") },
            { value: 2, label: t("game.display.touchNever") },
          ]}
          onChange={(touch) => set({ touch })}
        />
      )}
      {android && s.touch !== 2 && (
        <SelectRow
          title={t("game.display.touchButtons")}
          description={t("game.display.touchButtonsText")}
          value={layout}
          options={touchLayouts}
          onChange={chooseLayout}
        />
      )}
      {android && s.touch !== 2 && layout === "custom" && (
        <TouchEditor game={game} spec={s.touch_buttons} onChange={(touch_buttons) => set({ touch_buttons })} />
      )}
    </Section>
  );
}

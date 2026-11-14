import { useTranslation } from "react-i18next";
import { Section, SelectRow } from "@/components/common";
import type { GameSettings } from "@/lib";

/** Log detail and extra X2_* switches; the switches are saved when the field loses focus. */
export function AdvancedSettings({ s, set, edit }: { s: GameSettings; set: (patch: Partial<GameSettings>) => void; edit: (patch: Partial<GameSettings>) => void }) {
  const { t } = useTranslation();
  return (
    <Section title={t("game.advanced.title")}>
      <SelectRow
        title={t("game.advanced.log")}
        value={s.log_level}
        options={[
          { value: 0, label: t("game.advanced.logNormal") },
          { value: 1, label: t("game.advanced.logVerbose") },
          { value: 2, label: t("game.advanced.logMore") },
          { value: 3, label: t("game.advanced.logAll") },
        ]}
        onChange={(log_level) => set({ log_level })}
      />
      <div className="px-5 pb-4 pt-2">
        <div className="text-sm font-medium">{t("game.advanced.env")}</div>
        <div className="mb-2 mt-0.5 text-xs text-muted-foreground">{t("game.advanced.envText")}</div>
        <textarea
          className="h-24 w-full resize-y rounded-lg border border-input bg-secondary px-3 py-2 font-mono text-xs outline-none focus-visible:border-ring focus-visible:ring-3 focus-visible:ring-ring/50"
          value={s.env}
          spellCheck={false}
          placeholder="X2_TOUCH=0"
          onChange={(e) => edit({ env: e.target.value })}
          onBlur={() => set({})}
        />
      </div>
    </Section>
  );
}
